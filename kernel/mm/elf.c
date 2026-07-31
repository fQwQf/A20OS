/*
 * A20OS ELF64 Loader
 *
 * Loads ELF executables into a new per-process address space with:
 *   - User segments mapped at ELF virtual addresses (PTE_USER)
 *   - Kernel space identity-mapped (PTE_KERN)
 *   - User stack at VA 0x3FFFF000 downward
 *   - Optional PT_INTERP dynamic linker loading
 *   - TLS/TCB setup for musl compatibility
 */

#include "mm/elf.h"
#include "mm/mm.h"
#include "mm/vm.h"
#include "fs/vfs.h"
#include "fs/pathutil.h"
#include "core/string.h"
#include "core/stdio.h"
#include "core/panic.h"
#include "core/consts.h"
#include "core/defs.h"
#include "core/klog.h"
#include "core/random.h"
#include "proc/signal.h"
#include "mm/frame.h"
#ifdef CONFIG_ABI_NATIVE
#include "abi/native/startup.h"
#endif

/* musl struct pthread is ~300-400 bytes; 512 gives headroom */
#define TLS_TCB_SIZE       512

#define ASLR_BITS          11
#define ASLR_MASK          (((1UL << ASLR_BITS) - 1) << 16)

/* Max program headers we read in one shot (3584 bytes on stack) */
#define MAX_PHDRS          64

#ifdef CONFIG_NOMMU
static void elf_transfer_nommu_allocs(mm_struct_t *mm, elf_load_info_t *info) {
    info->num_nommu_allocs = mm->num_nommu_allocs;
    for (int i = 0; i < mm->num_nommu_allocs && i < NOMMU_ALLOC_MAX; i++) {
        info->nommu_allocs[i] = mm->nommu_allocs[i];
        info->nommu_alloc_sizes[i] = mm->nommu_alloc_sizes[i];
        info->nommu_alloc_types[i] = mm->nommu_alloc_types[i];
    }
    mm->num_nommu_allocs = 0;
}

static int elf_alloc_nommu_image(mm_struct_t *mm, vaddr_t min_vaddr,
                                 vaddr_t max_vaddr, vaddr_t *load_bias_out) {
    *load_bias_out = 0;
    if (max_vaddr <= min_vaddr)
        return 0;

    size_t image_size = (size_t)(max_vaddr - min_vaddr);
    size_t alloc_size = image_size + PAGE_SIZE;
    void *mem = kmalloc(alloc_size);
    if (!mem)
        return -ENOMEM;

    mm_track_nommu_alloc(mm, mem, alloc_size, NOMMU_ALLOC_IMAGE);
    vaddr_t base = ROUND_UP((vaddr_t)mem, PAGE_SIZE);
    memset((void *)base, 0, image_size);
    *load_bias_out = base - min_vaddr;
    return 0;
}
#endif

/* ------------------------------------------------------------------ */
/*  Utilities                                                         */
/* ------------------------------------------------------------------ */

static uint64_t elf_aslr_bias(void) {
    uint64_t r = random_u64();
    uint64_t bits = (r ^ (r >> 29) ^ (r >> 47)) & ((1UL << ASLR_BITS) - 1);
    return bits << 16;
}

static int elf_machine_supported(uint16_t machine, int elf_class) {
    return elf_class == ARCH_ELF_CLASS && machine == ARCH_ELF_MACHINE;
}

static uint64_t seg_flags(uint32_t p_flags) {
    int prot = 0;
    if (p_flags & PF_R) prot |= 1;
    if (p_flags & PF_W) prot |= 2;
    if (p_flags & PF_X) prot |= 4;
    return mm_prot_to_pte_flags(prot);
}

static uint64_t pte_to_vm_flags(pte_t pte_flags) {
    return VM_ANON | mm_pte_flags_to_vm_flags(pte_flags);
}

static int elf_add_vma(mm_struct_t *mm, vaddr_t start, vaddr_t end,
                       uint64_t vm_flags, pte_t pte_flags) {
    if (!mm) return 0;
    vm_area_t *vma = kcalloc(1, sizeof(vm_area_t));
    if (!vma) return -ENOMEM;
    vma->start     = start;
    vma->end       = end;
    vma->vm_flags  = vm_flags;
    vma->pte_flags = pte_flags;
    vma->file_fd   = -1;
    mm_insert_vma(mm, vma);
    mm->total_vm += (end - start) / PAGE_SIZE;
    return 0;
}

static void elf_discard_vmas(mm_struct_t *mm)
{
    vm_area_t *vma = mm->mmap;
    while (vma) {
        vm_area_t *next = vma->next;
        if ((vma->vm_flags & VM_FILE) && vma->file_fd >= 0) {
            if (vma->file_vnode)
                vnode_put(vma->file_vnode);
            vfs_close(vma->file_fd);
        }
        kfree(vma);
        vma = next;
    }
    mm->mmap = NULL;
}

void elf_load_info_discard(elf_load_info_t *info)
{
    if (!info)
        return;
    mm_struct_t mm = { .mmap = info->mmap, .pgdir = info->pgdir };
    elf_discard_vmas(&mm);
    if (info->pgdir)
        pt_destroy_user(info->pgdir);
#ifdef CONFIG_NOMMU
    for (int i = 0; i < info->num_nommu_allocs; i++)
        kfree(info->nommu_allocs[i]);
#endif
    memset(info, 0, sizeof(*info));
}

static void *phys_for_va(pt_root_t *pgdir, vaddr_t va) {
    paddr_t pa = pt_translate(pgdir, va);
    if (pa == 0) return NULL;
    return (void *)((uintptr_t)pa + PAGE_OFFSET);
}

/*
 * 确保 sp_va 所在的页面已映射到页表中。
 * 如果 sp_va 低于当前栈底，则分配新的物理页并映射。
 * 这是为了防止 execve 时参数/环境变量过大导致栈溢出，
 * 写入未映射地址时页表脏数据被当作物理地址引发崩溃。
 *
 * @pgdir         页表根指针
 * @sp_va         需要访问的虚拟地址
 * @stack_bottom  当前栈底（传入指针，会被更新）
 * @max_grow      最多向下扩展多少页
 * @return        映射后的物理地址（内核直映射），或 NULL 失败
 */
static void *stack_ensure_mapped(pt_root_t *pgdir, vaddr_t sp_va,
                                 vaddr_t *stack_bottom, int max_grow) {
    vaddr_t page_va = sp_va & ~(vaddr_t)(PAGE_SIZE - 1);
    while (page_va < *stack_bottom && max_grow > 0) {
        void *frame = frame_alloc();
        if (!frame) return NULL;
        memset(frame, 0, PAGE_SIZE);
        vaddr_t map_va = *stack_bottom - PAGE_SIZE;
        int r = pt_map(pgdir, map_va, va_to_pa(frame),
                       mm_user_stack_pte_flags());
        if (r < 0) { frame_free(frame); return NULL; }
        *stack_bottom -= PAGE_SIZE;
        max_grow--;
    }
    return phys_for_va(pgdir, sp_va);
}

static int stack_copy(pt_root_t *pgdir, vaddr_t dst_va, const void *src,
                      size_t len, vaddr_t *stack_bottom)
{
    /* User-stack virtual pages need not be physically adjacent. */
    const char *from = src;
    while (len > 0) {
        void *dst = stack_ensure_mapped(pgdir, dst_va, stack_bottom, 64);
        if (!dst)
            return -ENOMEM;
        size_t chunk = PAGE_SIZE - (size_t)(dst_va & (PAGE_SIZE - 1));
        if (chunk > len)
            chunk = len;
        memcpy(dst, from, chunk);
        dst_va += chunk;
        from += chunk;
        len -= chunk;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Segment source abstraction                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    enum { SEG_BUF, SEG_FD } kind;
    union {
        struct { const void *data; } buf;
        struct { int fd; long offset; } fd;
    };
} seg_src_t;

static inline seg_src_t seg_from_buf(const void *data) {
    return (seg_src_t){ .kind = SEG_BUF, .buf = { .data = data } };
}

static inline seg_src_t seg_from_fd(int fd, long offset) {
    return (seg_src_t){ .kind = SEG_FD, .fd = { .fd = fd, .offset = offset } };
}

#ifndef CONFIG_NOMMU
/*
 * Install an fd-backed PT_LOAD as a demand-paged private mapping.
 *
 * Complete file pages stay file-backed.  A partial p_filesz tail is copied
 * into one zeroed anonymous page so bytes between p_filesz and p_memsz obey
 * ELF BSS semantics even when the backing file contains unrelated data there.
 * Remaining BSS pages are anonymous demand mappings.
 */
static int map_fd_segment_lazy(mm_struct_t *mm, pt_root_t *pgdir,
                               vaddr_t va, uint64_t memsz,
                               int fd, uint64_t file_offset,
                               uint64_t filesz, pte_t flags)
{
    vaddr_t start = va & ~(vaddr_t)(PAGE_SIZE - 1);
    vaddr_t mem_end = va + memsz;
    vaddr_t end = ROUND_UP(mem_end, PAGE_SIZE);
    uint64_t file_page_offset =
        file_offset & ~(uint64_t)(PAGE_SIZE - 1);

    if (filesz > memsz)
        return -ENOEXEC;
    if ((file_offset & (PAGE_SIZE - 1)) != (va & (PAGE_SIZE - 1)) ||
        mem_end < va || va + filesz < va)
        return -EINVAL;

    vaddr_t file_end = va + filesz;
    vaddr_t file_map_end = ROUND_DOWN(file_end, PAGE_SIZE);
    int prot = mm_pte_flags_to_prot(flags);

    /*
     * Adjacent PT_LOAD entries may share a page.  MAP_FIXED would replace
     * the earlier entry, whereas the eager loader preserves and overlays its
     * contents.  Keep that established behaviour by selecting the eager path
     * before installing any lazy VMA whenever this segment overlaps one.
     */
    for (vm_area_t *vma = mm->mmap; vma; vma = vma->next) {
        if (vma->start < end && vma->end > start)
            return -EINVAL;
    }

    if (file_map_end > start) {
        vaddr_t mapped = mm_mmap_file(mm, start, file_map_end - start,
                                      prot, MAP_PRIVATE | MAP_FIXED, fd,
                                      file_page_offset);
        if ((intptr_t)mapped < 0)
            return (int)(intptr_t)mapped;
    }

    vaddr_t anon_start;
    if (file_end > file_map_end) {
        vaddr_t page = file_map_end;
        void *frame = frame_alloc();
        if (!frame)
            return -ENOMEM;
        memset(frame, 0, PAGE_SIZE);

        vaddr_t copy_start = page < va ? va : page;
        uint64_t src_off = copy_start - va;
        uint64_t to_copy = filesz - src_off;
        size_t copy_off = (size_t)(copy_start - page);
        if (to_copy > PAGE_SIZE - copy_off)
            to_copy = PAGE_SIZE - copy_off;
        int nr = vfs_pread(fd, (char *)frame + copy_off, (size_t)to_copy,
                           file_offset + src_off);
        if (nr < 0 || (uint64_t)nr != to_copy) {
            frame_free(frame);
            return nr < 0 ? nr : -ENOEXEC;
        }
        int r = pt_map(pgdir, page, va_to_pa(frame), flags);
        if (r < 0) {
            frame_free(frame);
            return r;
        }
        if (flags & PTE_X)
            arch_flush_icache_range(frame, PAGE_SIZE);
        r = elf_add_vma(mm, page, page + PAGE_SIZE,
                        pte_to_vm_flags(flags), flags);
        if (r < 0)
            return r;
        mm->rss++;
        anon_start = page + PAGE_SIZE;
    } else {
        anon_start = file_end;
    }

    anon_start = ROUND_UP(anon_start, PAGE_SIZE);
    if (anon_start < end)
        return elf_add_vma(mm, anon_start, end,
                           pte_to_vm_flags(flags), flags);
    return 0;
}
#endif

/*
 * Map an ELF segment into the page table.
 *
 * Handles both buffer-backed and fd-backed segments through @src.
 * - va:      target virtual address (may be page-unaligned)
 * - memsz:   total memory size of segment (BSS included)
 * - filesz:  size of initialized data in file
 * - flags:   PTE flags for mapping
 */
static int map_segment(mm_struct_t *mm, pt_root_t *pgdir,
                       vaddr_t va, uint64_t memsz,
                       const seg_src_t *src, uint64_t filesz,
                       pte_t flags) {
    vaddr_t start = va & ~(vaddr_t)(PAGE_SIZE - 1);
    vaddr_t end   = ROUND_UP(va + memsz, PAGE_SIZE);
    char tmp[PAGE_SIZE];

#ifdef CONFIG_NOMMU
    /* In NOMMU, va is the physical address. We assume the system/loader 
     * has ensured this memory is available for this segment. */
    if (src->kind == SEG_BUF) {
        memcpy((void *)va, src->buf.data, filesz);
    } else {
        int nr = vfs_pread(src->fd.fd, (void *)va, filesz, src->fd.offset);
        if (nr != (int)filesz)
            kerr("[ELF] short read: %d/%lu\n", nr, (unsigned long)filesz);
        if (nr < 0) return nr;
    }
    if (memsz > filesz) {
        memset((void *)(va + filesz), 0, memsz - filesz);
    }
    return 0;
#else
    if (src->kind == SEG_FD) {
        int r = map_fd_segment_lazy(mm, pgdir, va, memsz, src->fd.fd,
                                    (uint64_t)src->fd.offset, filesz, flags);
        if (r != -EINVAL)
            return r;
        /* Non-conforming offset/vaddr alignment retains the eager fallback. */
    }

    for (vaddr_t page = start; page < end; page += PAGE_SIZE) {
        void *frame = frame_alloc();
        if (!frame) return -ENOMEM;

        /* Preserve existing page contents if page already mapped */
        pte_t *old_pte = pt_walk(pgdir, page, 0);
        if (old_pte && (*old_pte & PTE_V)) {
            paddr_t old_pa = arch_pte_addr(*old_pte);
            memcpy(frame, (void *)(old_pa + PAGE_OFFSET), PAGE_SIZE);
        } else {
            /*
             * PT_LOAD pages include the segment's zero-filled tail.  Frames
             * returned by the physical allocator retain prior contents, so
             * copying only p_filesz otherwise exposes stale data as .bss.
             */
            memset(frame, 0, PAGE_SIZE);
        }

        if (page < va + filesz) {
            vaddr_t copy_off = (page < va) ? (va - page) : 0;
            uint64_t src_off  = (page < va) ? 0 : (uint64_t)(page - va);
            uint64_t to_copy  = filesz - src_off;
            if (to_copy > PAGE_SIZE - copy_off)
                to_copy = PAGE_SIZE - copy_off;
            if (to_copy > 0) {
                if (src->kind == SEG_BUF) {
                    memcpy((char *)frame + copy_off,
                           (const char *)src->buf.data + src_off, to_copy);
                } else {
                    int nr = vfs_pread(src->fd.fd, tmp, (size_t)to_copy,
                                       (uint64_t)(src->fd.offset + (long)src_off));
                    if (nr < 0) { frame_free(frame); return nr; }
                    if ((uint64_t)nr != to_copy) {
                        frame_free(frame);
                        kerr("[ELF] short segment read: %d/%lu\n", nr,
                             (unsigned long)to_copy);
                        return -ENOEXEC;
                    }
                    memcpy((char *)frame + copy_off, tmp, (size_t)nr);
                }
            }
        }

        int r = pt_map(pgdir, page, va_to_pa(frame), flags);
        if (r < 0) { frame_free(frame); return r; }

        /*
         * The loader populates a user page through its kernel direct-map
         * alias.  On non-coherent I/D-cache implementations, invalidating
         * the I-cache later is not sufficient: the new instructions may
         * still only exist in dirty D-cache lines under this alias.  Clean
         * and invalidate executable pages while the address used for the
         * writes is available.  This is required by real AArch64 hardware
         * and VirtualBox even though QEMU's cache model often hides it.
         */
        if (flags & PTE_X)
            arch_flush_icache_range(frame, PAGE_SIZE);
    }
    arch_tlb_flush();
    return elf_add_vma(mm, start, end, pte_to_vm_flags(flags), flags);
#endif
}

/* ------------------------------------------------------------------ */
/*  Stack mapping                                                     */
/* ------------------------------------------------------------------ */

static int map_stack(mm_struct_t *mm, pt_root_t *pgdir, vaddr_t *stack_top_out) {
#ifdef CONFIG_NOMMU
    size_t stack_size = (uint64_t)USER_STACK_INITIAL_PAGES * PAGE_SIZE;
    size_t stack_alloc_size = stack_size + PAGE_SIZE;
    void *stack_alloc = kmalloc(stack_alloc_size);
    if (!stack_alloc) return -ENOMEM;
    memset(stack_alloc, 0, stack_alloc_size);
    mm_track_nommu_alloc(mm, stack_alloc, stack_alloc_size, NOMMU_ALLOC_STACK);
    /*
     * Big kmalloc allocations carry an allocator header before the returned
     * pointer, so the pointer itself is not page aligned.  Keep user SP and
     * all ABI stack objects naturally aligned by reserving one extra page.
     */
    vaddr_t stack_bottom = ROUND_UP((vaddr_t)stack_alloc, PAGE_SIZE);
    vaddr_t stack_top = stack_bottom + stack_size;
    *stack_top_out = stack_top;
    return elf_add_vma(mm, stack_bottom, stack_top,
                       VM_ANON | VM_READ | VM_WRITE | VM_STACK,
                       mm_user_stack_pte_flags());
#else
    vaddr_t stack_top    = USER_STACK_TOP + PAGE_SIZE;
    vaddr_t stack_bottom = stack_top -
        (uint64_t)USER_STACK_INITIAL_PAGES * PAGE_SIZE;

    for (int i = 0; i < USER_STACK_INITIAL_PAGES; i++) {
        vaddr_t va = USER_STACK_TOP -
                      (uint64_t)(USER_STACK_INITIAL_PAGES - 1 - i) * PAGE_SIZE;
        void *frame = frame_alloc();
        if (!frame) return -ENOMEM;
        memset(frame, 0, PAGE_SIZE);
        int r = pt_map(pgdir, va, va_to_pa(frame),
                       mm_user_stack_pte_flags());
        if (r < 0) { frame_free(frame); return r; }
    }
    *stack_top_out = stack_top;
    arch_tlb_flush();
    return elf_add_vma(mm, stack_bottom, stack_top,
                       VM_ANON | VM_READ | VM_WRITE | VM_STACK,
                       mm_user_stack_pte_flags());
#endif
}

/* ------------------------------------------------------------------ */
/*  TLS setup                                                         */
/* ------------------------------------------------------------------ */

static int setup_tls(mm_struct_t *mm, pt_root_t *pgdir,
                     const void *tls_data, uint64_t tls_filesz,
                     uint64_t tls_memsz, uint64_t tls_align,
                     vaddr_t *tls_va_out, vaddr_t *tls_tp_out) {
    uint64_t tcb_offset = ROUND_UP(tls_memsz, tls_align);
    uint64_t total_size = tcb_offset + TLS_TCB_SIZE;
    uint64_t total_pages = ROUND_UP(total_size, PAGE_SIZE) / PAGE_SIZE;
    pte_t pte_flags = mm_user_brk_pte_flags();

#ifdef CONFIG_NOMMU
    void *tls_mem = kmalloc(total_pages * PAGE_SIZE);
    if (!tls_mem) return -ENOMEM;
    mm_track_nommu_alloc(mm, tls_mem, total_pages * PAGE_SIZE, NOMMU_ALLOC_TLS);
    vaddr_t tls_va = (vaddr_t)tls_mem;
    *tls_va_out = tls_va;
    memset(tls_mem, 0, total_pages * PAGE_SIZE);
    if (tls_data && tls_filesz > 0) {
        memcpy(tls_mem, tls_data, tls_filesz);
    }
#else
    for (uint64_t page = 0; page < total_pages; page++) {
        void *frame = frame_alloc();
        if (!frame) return -ENOMEM;
        int r = pt_map(pgdir, USER_TLS_BASE + page * PAGE_SIZE,
                       va_to_pa(frame), pte_flags);
        if (r < 0) { frame_free(frame); return r; }
    }

    vaddr_t tls_va = USER_TLS_BASE;
    if (tls_data && tls_filesz > 0) {
        for (uint64_t off = 0; off < tls_filesz; ) {
            void *dst = phys_for_va(pgdir, tls_va + off);
            if (!dst) return -EFAULT;
            uint64_t chunk = tls_filesz - off;
            uint64_t page_off = (tls_va + off) & (PAGE_SIZE - 1);
            if (chunk > PAGE_SIZE - page_off)
                chunk = PAGE_SIZE - page_off;
            memcpy(dst, (const char *)tls_data + off, chunk);
            off += chunk;
        }
    }
#endif

    vaddr_t tcb_va = tls_va + tcb_offset;
#ifdef CONFIG_NOMMU
    void *tcb_dst = (void *)tcb_va;
#else
    void *tcb_dst = phys_for_va(pgdir, tcb_va);
    if (!tcb_dst) return -EFAULT;
    memset(tcb_dst, 0, TLS_TCB_SIZE);
#endif

    if (sizeof(uintptr_t) == 4) {
        *(uint32_t *)tcb_dst       = (uint32_t)tcb_va;
        *((uint32_t *)tcb_dst + 1) = (uint32_t)tcb_va;
    } else {
        *(uint64_t *)tcb_dst       = (uint64_t)tcb_va;
        *((uint64_t *)tcb_dst + 1) = (uint64_t)tcb_va;
    }

    *tls_va_out = tls_va;
    *tls_tp_out = tcb_va;
    arch_tlb_flush();
    return elf_add_vma(mm, tls_va,
                       tls_va + total_pages * PAGE_SIZE,
                       VM_ANON | VM_READ | VM_WRITE, pte_flags);
}

/* ------------------------------------------------------------------ */
/*  Interp (dynamic linker) resolution                                */
/* ------------------------------------------------------------------ */

/*
 * Try to open the ELF interpreter.  Returns opened fd >= 0, or negative errno.
 *
 * Resolution order:
 *   1. Exact PT_INTERP path
 *   2. Interpreter relative to the executable's mount root
 *   3. Sibling of executable (strip last path component, append interp)
 *   4. Sibling libc.so (musl convention: libc.so IS the dynamic linker)
 *   5. Arch-specific fallbacks
 *
 * On success, @resolved is set to the path that worked.
 */
static int resolve_interp(const char *exec_path, const char *interp_path,
                          char *resolved, size_t resolved_size) {
    /* 1. Exact path */
    int fd = vfs_open(interp_path, O_RDONLY, 0);
    if (fd >= 0) {
        strncpy(resolved, interp_path, resolved_size - 1);
        resolved[resolved_size - 1] = '\0';
        return fd;
    }

    /* 2. Mount-relative path.  A20OS commonly mounts a userspace image at
     * /bin, so /bin/libexec/tool must resolve /lib/ld.so as /bin/lib/ld.so,
     * just like /bin/tool does. */
    if (exec_path && path_build_mount_relative(exec_path, interp_path,
                                                resolved, resolved_size) == 0) {
        fd = vfs_open(resolved, O_RDONLY, 0);
        if (fd >= 0) return fd;
    }

    /* 3. Sibling of executable */
    if (exec_path && path_build_sibling(exec_path, interp_path,
                                        resolved, resolved_size) == 0) {
        fd = vfs_open(resolved, O_RDONLY, 0);
        if (fd >= 0) return fd;
    }

    /* 4. Sibling libc.so (musl: libc.so doubles as ldso) */
    if (exec_path && path_build_sibling(exec_path, "/lib/libc.so",
                                        resolved, resolved_size) == 0) {
        fd = vfs_open(resolved, O_RDONLY, 0);
        if (fd >= 0) return fd;
    }

    /* 5. Arch-specific fallbacks */
    char alt[MAX_PATH_LEN];
    if (arch_resolve_interp_fallback(exec_path, interp_path,
                                     alt, sizeof(alt)) == 0) {
        fd = vfs_open(alt, O_RDONLY, 0);
        if (fd >= 0) {
            strncpy(resolved, alt, resolved_size - 1);
            resolved[resolved_size - 1] = '\0';
            return fd;
        }
    }

    return -ENOENT;
}

/*
 * Load the dynamic linker from an already-opened fd.
 * The caller is responsible for closing @fd on failure.
 */
static int elf_load_interp_from_fd(mm_struct_t *mm, pt_root_t *pgdir,
                                    int fd,
                                    vaddr_t *entry_out, vaddr_t *base_out) {
    Elf64_Ehdr eh;
    if (vfs_lseek(fd, 0, SEEK_SET) < 0 ||
        vfs_read(fd, (char *)&eh, sizeof(eh)) < (int)sizeof(eh))
        return -ENOEXEC;

    int r = elf_check_header(&eh);
    if (r < 0) return r;

    Elf64_Phdr phdrs[MAX_PHDRS];
    int nph = eh.e_phnum < MAX_PHDRS ? eh.e_phnum : MAX_PHDRS;
    vfs_lseek(fd, (long)eh.e_phoff, SEEK_SET);
    r = vfs_read(fd, (char *)phdrs, nph * sizeof(Elf64_Phdr));
    if (r < nph * (int)sizeof(Elf64_Phdr)) return -ENOEXEC;

#ifdef CONFIG_NOMMU
    vaddr_t min_vaddr = (vaddr_t)-1;
    vaddr_t max_vaddr = 0;
    for (int i = 0; i < nph; i++) {
        if (phdrs[i].p_type != PT_LOAD) continue;
        vaddr_t s = phdrs[i].p_vaddr & ~(vaddr_t)(PAGE_SIZE - 1);
        vaddr_t e = ROUND_UP(phdrs[i].p_vaddr + phdrs[i].p_memsz, PAGE_SIZE);
        if (s < min_vaddr) min_vaddr = s;
        if (e > max_vaddr) max_vaddr = e;
    }
    vaddr_t load_bias = 0;
    r = elf_alloc_nommu_image(mm, min_vaddr, max_vaddr, &load_bias);
    if (r < 0) return r;
#else
    vaddr_t load_bias = INTERP_BASE_ADDR + elf_aslr_bias();
#endif
    vaddr_t base = 0, max_va = 0;

    for (int i = 0; i < nph; i++) {
        if (phdrs[i].p_type != PT_LOAD) continue;

        vaddr_t seg_va = phdrs[i].p_vaddr + load_bias;
        seg_src_t src = seg_from_fd(fd, (long)phdrs[i].p_offset);
        r = map_segment(mm, pgdir, seg_va, phdrs[i].p_memsz,
                        &src, phdrs[i].p_filesz,
                        seg_flags(phdrs[i].p_flags));
        if (r < 0) return r;

        vaddr_t seg_start = seg_va & ~(vaddr_t)(PAGE_SIZE - 1);
        vaddr_t seg_end   = ROUND_UP(seg_va + phdrs[i].p_memsz, PAGE_SIZE);
#ifdef CONFIG_NOMMU
        if (base == 0) base = load_bias + min_vaddr;
#else
        if (base == 0) base = seg_start;
#endif
        if (seg_end > max_va) max_va = seg_end;
    }

#ifdef CONFIG_NOMMU
    if (max_va > base) {
        elf_add_vma(mm, base, max_va, VM_ANON | VM_READ | VM_WRITE | VM_EXEC, mm_user_brk_pte_flags());
    }
#endif

    *entry_out = eh.e_entry + load_bias;
    *base_out  = base;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

int elf_check_header(const Elf64_Ehdr *eh) {
    if (*(uint32_t *)eh->e_ident != ELF_MAGIC) return -ENOEXEC;
    if (eh->e_ident[4] != ELFCLASS64)           return -ENOEXEC;
    if (eh->e_ident[5] != ELFDATA2LSB)          return -ENOEXEC;
    if (eh->e_type != ET_EXEC && eh->e_type != ET_DYN) return -ENOEXEC;
    if (!elf_machine_supported(eh->e_machine, ELFCLASS64)) return -ENOEXEC;
    if (eh->e_phentsize < sizeof(Elf64_Phdr))   return -ENOEXEC;
    if (eh->e_phnum == 0 || eh->e_phnum > MAX_PHDRS) return -ENOEXEC;
    return 0;
}

int elf_load_from_buf(const void *buf, size_t len, elf_load_info_t *info) {
    if (len < sizeof(Elf64_Ehdr)) return -ENOEXEC;
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)buf;

    int r = elf_check_header(eh);
    if (r < 0) return r;

    pt_root_t *pgdir = pt_create();
    if (!pgdir) return -ENOMEM;
    pt_map_kernel(pgdir);

    mm_struct_t mm = { .pgdir = pgdir, .mmap_base = MMAP_BASE_ADDR };

#ifdef CONFIG_NOMMU
    vaddr_t min_vaddr = (vaddr_t)-1;
    vaddr_t max_vaddr = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (eh->e_phoff + (i + 1) * eh->e_phentsize > len) continue;
        const Elf64_Phdr *ph = (const Elf64_Phdr *)((const char *)buf + eh->e_phoff + i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD) continue;
        vaddr_t s = ph->p_vaddr & ~(vaddr_t)(PAGE_SIZE - 1);
        vaddr_t e = ROUND_UP(ph->p_vaddr + ph->p_memsz, PAGE_SIZE);
        if (s < min_vaddr) min_vaddr = s;
        if (e > max_vaddr) max_vaddr = e;
    }
    vaddr_t load_bias = 0;
    r = elf_alloc_nommu_image(&mm, min_vaddr, max_vaddr, &load_bias);
    if (r < 0) return r; // FIXME: leaks pgdir
#else
    vaddr_t load_bias = (eh->e_type == ET_DYN) ? (USER_DYN_BASE + elf_aslr_bias()) : 0;
#endif
    vaddr_t base = 0, max_va = 0, brk_va = 0;
    const void *tls_data = NULL;
    uint64_t tls_filesz = 0, tls_memsz = 0, tls_align = 1;
    int is_native = 0;

    for (int i = 0; i < eh->e_phnum; i++) {
        if (eh->e_phoff + (i + 1) * eh->e_phentsize > len) continue;
        const Elf64_Phdr *ph = (const Elf64_Phdr *)
            ((const char *)buf + eh->e_phoff + i * eh->e_phentsize);

        if (ph->p_type == PT_A20_START_INFO) {
            is_native = 1;
            continue;
        }
        if (ph->p_type == PT_TLS) {
            tls_data   = (const char *)buf + ph->p_offset;
            tls_filesz = ph->p_filesz;
            tls_memsz  = ph->p_memsz;
            tls_align  = ph->p_align < 1 ? 1 : ph->p_align;
            continue;
        }
        if (ph->p_type != PT_LOAD) continue;

        vaddr_t seg_va = ph->p_vaddr + load_bias;
        seg_src_t src = seg_from_buf((const char *)buf + ph->p_offset);
        r = map_segment(&mm, pgdir, seg_va, ph->p_memsz,
                        &src, ph->p_filesz, seg_flags(ph->p_flags));
        if (r < 0) { pt_destroy_user(pgdir); return r; }

        vaddr_t seg_start = seg_va & ~(vaddr_t)(PAGE_SIZE - 1);
        vaddr_t seg_end   = ROUND_UP(seg_va + ph->p_memsz, PAGE_SIZE);
#ifdef CONFIG_NOMMU
        if (base == 0) base = load_bias + min_vaddr;
#else
        if (base == 0) base = seg_start;
#endif
        if (seg_end > max_va) max_va = seg_end;
        if ((ph->p_flags & PF_W) && seg_end > brk_va) brk_va = seg_end;
    }

#ifdef CONFIG_NOMMU
    if (max_va > base) {
        elf_add_vma(&mm, base, max_va, VM_ANON | VM_READ | VM_WRITE | VM_EXEC, mm_user_brk_pte_flags());
    }
#endif

    vaddr_t stack_top;
    r = map_stack(&mm, pgdir, &stack_top);
    if (r < 0) { pt_destroy_user(pgdir); return r; }

    vaddr_t tls_va = 0, tls_tp = 0;
    r = setup_tls(&mm, pgdir, tls_data, tls_filesz, tls_memsz, tls_align,
                  &tls_va, &tls_tp);
    if (r < 0) { pt_destroy_user(pgdir); return r; }

    *info = (elf_load_info_t){
        .entry       = eh->e_entry + load_bias,
        .exec_entry  = eh->e_entry + load_bias,
        .base        = base,
        .end_va      = max_va,
        .brk         = ROUND_UP(brk_va ? brk_va : max_va, PAGE_SIZE),
        .phdr_va     = base + eh->e_phoff,
        .phnum       = eh->e_phnum,
        .phentsize   = eh->e_phentsize,
        .load_addr   = base,
        .load_size   = (size_t)(max_va - base),
        .pgdir       = pgdir,
        .stack_top   = stack_top,
        .tls_va      = tls_va,
        .tls_size    = tls_memsz,
        .tls_tp      = tls_tp,
        .interp_base = 0,
        .mmap        = mm.mmap,
        .is_native_abi = is_native,
    };
#ifdef CONFIG_NOMMU
    elf_transfer_nommu_allocs(&mm, info);
#endif
    return 0;
}

static int elf_load64(int fd, const Elf64_Ehdr *eh, const char *path,
                      elf_load_info_t *info) {
    Elf64_Phdr phdrs[MAX_PHDRS];
    int nph = eh->e_phnum < MAX_PHDRS ? eh->e_phnum : MAX_PHDRS;
    vfs_lseek(fd, (long)eh->e_phoff, SEEK_SET);
    int r = vfs_read(fd, (char *)phdrs, nph * sizeof(Elf64_Phdr));
    if (r < nph * (int)sizeof(Elf64_Phdr)) return -ENOEXEC;

    pt_root_t *pgdir = pt_create();
    if (!pgdir) return -ENOMEM;
    pt_map_kernel(pgdir);

    mm_struct_t mm = { .pgdir = pgdir, .mmap_base = MMAP_BASE_ADDR };

#ifdef CONFIG_NOMMU
    vaddr_t min_vaddr = (vaddr_t)-1;
    vaddr_t max_vaddr = 0;
    for (int i = 0; i < nph; i++) {
        if (phdrs[i].p_type != PT_LOAD) continue;
        vaddr_t s = phdrs[i].p_vaddr & ~(vaddr_t)(PAGE_SIZE - 1);
        vaddr_t e = ROUND_UP(phdrs[i].p_vaddr + phdrs[i].p_memsz, PAGE_SIZE);
        if (s < min_vaddr) min_vaddr = s;
        if (e > max_vaddr) max_vaddr = e;
    }
    vaddr_t load_bias = 0;
    r = elf_alloc_nommu_image(&mm, min_vaddr, max_vaddr, &load_bias);
    if (r < 0) return r; // FIXME: leaks pgdir
#else
    vaddr_t load_bias = (eh->e_type == ET_DYN) ? (USER_DYN_BASE + elf_aslr_bias()) : 0;
#endif
    vaddr_t base = 0, max_va = 0, brk_va = 0, head_va = 0;
    void *tls_data = NULL;
    uint64_t tls_filesz = 0, tls_memsz = 0, tls_align = 1;
    int has_interp = 0;
    int is_native = 0;
    char interp_path[MAX_PATH_LEN] = {0};

    for (int i = 0; i < nph; i++) {
        if (phdrs[i].p_type == PT_A20_START_INFO) {
            is_native = 1;
            continue;
        }
        if (phdrs[i].p_type == PT_INTERP) {
            has_interp = 1;
            vfs_lseek(fd, (long)phdrs[i].p_offset, SEEK_SET);
            int ilen = phdrs[i].p_filesz < MAX_PATH_LEN
                       ? (int)phdrs[i].p_filesz : MAX_PATH_LEN - 1;
            vfs_read(fd, interp_path, (size_t)ilen);
            interp_path[ilen] = '\0';
            continue;
        }
        if (phdrs[i].p_type == PT_TLS) {
            tls_filesz = phdrs[i].p_filesz;
            tls_memsz  = phdrs[i].p_memsz;
            tls_align  = phdrs[i].p_align < 1 ? 1 : phdrs[i].p_align;
            if (tls_filesz > 0) {
                tls_data = kmalloc((size_t)tls_filesz);
                if (!tls_data) goto fail64;
                vfs_lseek(fd, (long)phdrs[i].p_offset, SEEK_SET);
                int nr = vfs_read(fd, (char *)tls_data, (size_t)tls_filesz);
                if (nr < 0) { kfree(tls_data); tls_data = NULL; goto fail64; }
            }
            continue;
        }
        if (phdrs[i].p_type != PT_LOAD) continue;

        vaddr_t seg_va = phdrs[i].p_vaddr + load_bias;
        seg_src_t src = seg_from_fd(fd, (long)phdrs[i].p_offset);
        r = map_segment(&mm, pgdir, seg_va, phdrs[i].p_memsz,
                        &src, phdrs[i].p_filesz,
                        seg_flags(phdrs[i].p_flags));
        if (r < 0) goto fail64;

        vaddr_t seg_start = seg_va & ~(vaddr_t)(PAGE_SIZE - 1);
        vaddr_t seg_end   = ROUND_UP(seg_va + phdrs[i].p_memsz, PAGE_SIZE);
        if (phdrs[i].p_offset == 0) head_va = seg_va;
#ifdef CONFIG_NOMMU
        if (base == 0) base = load_bias + min_vaddr;
#else
        if (base == 0) base = seg_start;
#endif
        if (seg_end > max_va) max_va = seg_end;
        if ((phdrs[i].p_flags & PF_W) && seg_end > brk_va) brk_va = seg_end;
    }

#ifdef CONFIG_NOMMU
    if (max_va > base) {
        elf_add_vma(&mm, base, max_va, VM_ANON | VM_READ | VM_WRITE | VM_EXEC, mm_user_brk_pte_flags());
    }
#endif

    vaddr_t interp_entry = 0, interp_base = 0;
    if (has_interp) {
        char resolved[MAX_PATH_LEN];
        int interp_fd = resolve_interp(path, interp_path,
                                       resolved, sizeof(resolved));
        ktrace_mm("[ELF] interp: exec='%s' pt_interp='%s' resolved='%s' fd=%d\n",
                  path ? path : "(null)", interp_path, resolved, interp_fd);
        if (interp_fd < 0) {
            printf("[ELF] INTERP NOT FOUND for '%s' wanted '%s'\n",
                  path ? path : "(null)", interp_path);
            r = interp_fd;
            goto fail64;
        }
        r = elf_load_interp_from_fd(&mm, pgdir, interp_fd,
                                    &interp_entry, &interp_base);
        vfs_close(interp_fd);
        if (r < 0) goto fail64;
    }


    vaddr_t stack_top;
    r = map_stack(&mm, pgdir, &stack_top);
    if (r < 0) goto fail64;

    vaddr_t tls_va = 0, tls_tp = 0;
    r = setup_tls(&mm, pgdir, tls_data, tls_filesz, tls_memsz, tls_align,
                  &tls_va, &tls_tp);
    if (r < 0)
        goto fail64;

    *info = (elf_load_info_t){
        .entry       = has_interp ? interp_entry : (eh->e_entry + load_bias),
        .exec_entry  = eh->e_entry + load_bias,
        .base        = base,
        .end_va      = max_va,
        .brk         = ROUND_UP(brk_va ? brk_va : max_va, PAGE_SIZE),
        .phdr_va     = head_va ? (head_va + eh->e_phoff) : (base + eh->e_phoff),
        .phnum       = (uint32_t)nph,
        .phentsize   = eh->e_phentsize,
        .load_addr   = base,
        .load_size   = (size_t)(max_va - base),
        .pgdir       = pgdir,
        .stack_top   = stack_top,
        .tls_va      = tls_va,
        .tls_size    = tls_memsz,
        .tls_tp      = tls_tp,
        .interp_base = interp_base,
        .mmap        = mm.mmap,
        .is_native_abi = is_native,
    };
#ifdef CONFIG_NOMMU
    elf_transfer_nommu_allocs(&mm, info);
#endif

    kfree(tls_data);
    return 0;

fail64:
    kfree(tls_data);
    elf_discard_vmas(&mm);
    pt_destroy_user(pgdir);
    return r;
}

static int elf_load32(int fd, const Elf32_Ehdr *eh, const char *path,
                      elf_load_info_t *info) {
    (void)path;
    Elf32_Phdr phdrs[MAX_PHDRS];
    int nph = eh->e_phnum < MAX_PHDRS ? eh->e_phnum : MAX_PHDRS;
    vfs_lseek(fd, (long)eh->e_phoff, SEEK_SET);
    int r = vfs_read(fd, (char *)phdrs, nph * sizeof(Elf32_Phdr));
    if (r < nph * (int)sizeof(Elf32_Phdr)) return -ENOEXEC;

    pt_root_t *pgdir = pt_create();
    if (!pgdir) return -ENOMEM;
    pt_map_kernel(pgdir);

    mm_struct_t mm = { .pgdir = pgdir, .mmap_base = MMAP_BASE_ADDR };

#ifdef CONFIG_NOMMU
    vaddr_t min_vaddr = (vaddr_t)-1;
    vaddr_t max_vaddr = 0;
    for (int i = 0; i < nph; i++) {
        if (phdrs[i].p_type != PT_LOAD) continue;
        vaddr_t s = (vaddr_t)phdrs[i].p_vaddr & ~(vaddr_t)(PAGE_SIZE - 1);
        vaddr_t e = ROUND_UP((vaddr_t)phdrs[i].p_vaddr + (uint64_t)phdrs[i].p_memsz, PAGE_SIZE);
        if (s < min_vaddr) min_vaddr = s;
        if (e > max_vaddr) max_vaddr = e;
    }
    vaddr_t load_bias = 0;
    r = elf_alloc_nommu_image(&mm, min_vaddr, max_vaddr, &load_bias);
    if (r < 0) { pt_destroy_user(pgdir); return r; }
#else
    vaddr_t load_bias = (eh->e_type == ET_DYN) ? (USER_DYN_BASE + elf_aslr_bias()) : 0;
#endif
    vaddr_t base = 0, max_va = 0, brk_va = 0, head_va = 0;
    void *tls_data = NULL;
    uint64_t tls_filesz = 0, tls_memsz = 0, tls_align = 1;
    int is_native = 0;
    char interp_path[MAX_PATH_LEN] = {0};

    for (int i = 0; i < nph; i++) {
        if (phdrs[i].p_type == PT_A20_START_INFO) {
            is_native = 1;
            continue;
        }
        if (phdrs[i].p_type == PT_INTERP) {
            int ilen = phdrs[i].p_filesz < MAX_PATH_LEN
                       ? (int)phdrs[i].p_filesz : MAX_PATH_LEN - 1;
            vfs_lseek(fd, (long)phdrs[i].p_offset, SEEK_SET);
            vfs_read(fd, interp_path, (size_t)ilen);
            interp_path[ilen] = '\0';
            continue;
        }
        if (phdrs[i].p_type == PT_TLS) {
            tls_filesz = phdrs[i].p_filesz;
            tls_memsz  = phdrs[i].p_memsz;
            tls_align  = phdrs[i].p_align < 1 ? 1 : phdrs[i].p_align;
            if (tls_filesz > 0) {
                tls_data = kmalloc((size_t)tls_filesz);
                if (!tls_data) goto fail32;
                vfs_lseek(fd, (long)phdrs[i].p_offset, SEEK_SET);
                int nr = vfs_read(fd, (char *)tls_data, (size_t)tls_filesz);
                if (nr < 0) { kfree(tls_data); tls_data = NULL; goto fail32; }
            }
            continue;
        }
        if (phdrs[i].p_type != PT_LOAD) continue;

        vaddr_t seg_va = (vaddr_t)phdrs[i].p_vaddr + load_bias;
        seg_src_t src = seg_from_fd(fd, (long)phdrs[i].p_offset);
        r = map_segment(&mm, pgdir, seg_va, (uint64_t)phdrs[i].p_memsz,
                        &src, (uint64_t)phdrs[i].p_filesz,
                        seg_flags(phdrs[i].p_flags));
        if (r < 0) goto fail32;

        vaddr_t seg_start = seg_va & ~(vaddr_t)(PAGE_SIZE - 1);
        vaddr_t seg_end   = ROUND_UP(seg_va + (uint64_t)phdrs[i].p_memsz, PAGE_SIZE);
        if (phdrs[i].p_offset == 0) head_va = seg_va;
#ifdef CONFIG_NOMMU
        if (base == 0) base = load_bias + min_vaddr;
#else
        if (base == 0) base = seg_start;
#endif
        if (seg_end > max_va) max_va = seg_end;
        if ((phdrs[i].p_flags & PF_W) && seg_end > brk_va) brk_va = seg_end;
    }

    if (interp_path[0]) {
        r = -ENOEXEC;
        goto fail32;
    }

#ifdef CONFIG_NOMMU
    if (max_va > base) {
        elf_add_vma(&mm, base, max_va, VM_ANON | VM_READ | VM_WRITE | VM_EXEC, mm_user_brk_pte_flags());
    }
#endif

    vaddr_t stack_top;
    r = map_stack(&mm, pgdir, &stack_top);
    if (r < 0) goto fail32;

    vaddr_t tls_va = 0, tls_tp = 0;
    r = setup_tls(&mm, pgdir, tls_data, tls_filesz, tls_memsz, tls_align,
                  &tls_va, &tls_tp);
    if (r < 0)
        goto fail32;

    *info = (elf_load_info_t){
        .entry       = (vaddr_t)eh->e_entry + load_bias,
        .exec_entry  = (vaddr_t)eh->e_entry + load_bias,
        .base        = base,
        .end_va      = max_va,
        .brk         = ROUND_UP(brk_va ? brk_va : max_va, PAGE_SIZE),
        .phdr_va     = head_va ? (head_va + eh->e_phoff) : (base + eh->e_phoff),
        .phnum       = (uint32_t)nph,
        .phentsize   = eh->e_phentsize,
        .load_addr   = base,
        .load_size   = (size_t)(max_va - base),
        .pgdir       = pgdir,
        .stack_top   = stack_top,
        .tls_va      = tls_va,
        .tls_size    = tls_memsz,
        .tls_tp      = tls_tp,
        .interp_base = 0,
        .mmap        = mm.mmap,
        .is_native_abi = is_native,
    };
#ifdef CONFIG_NOMMU
    elf_transfer_nommu_allocs(&mm, info);
#endif

    kfree(tls_data);
    return 0;

fail32:
    kfree(tls_data);
    elf_discard_vmas(&mm);
    pt_destroy_user(pgdir);
    return r;
}

int elf_load(int fd, const char *path, elf_load_info_t *info) {
    unsigned char buf[sizeof(Elf64_Ehdr)];
    if (vfs_lseek(fd, 0, SEEK_SET) < 0) return -EIO;
    int r = vfs_read(fd, (char *)buf, sizeof(buf));
    if (r < (int)sizeof(Elf32_Ehdr)) {
        kinfo("[ELF] read header failed: r=%d need=%zu path='%s'\n",
              r, sizeof(Elf32_Ehdr), path ? path : "(null)");
        return -ENOEXEC;
    }

    if (*(uint32_t *)buf != ELF_MAGIC) return -ENOEXEC;
    int is32 = buf[4] == ELFCLASS32;
    int is64 = buf[4] == ELFCLASS64;
    if (!is32 && !is64) {
        printf("[ELF] unsupported ELF class=%d path='%s'\n", buf[4],
               path ? path : "(null)");
        return -ENOEXEC;
    }

    if (is64) {
        const Elf64_Ehdr *eh = (const Elf64_Ehdr *)buf;
        r = elf_check_header(eh);
        if (r < 0) {
            printf("[ELF] header check failed: r=%d class=%d data=%d type=%d\n",
                   r, eh->e_ident[4], eh->e_ident[5], eh->e_type);
            return r;
        }
        return elf_load64(fd, eh, path, info);
    }

    const Elf32_Ehdr *eh = (const Elf32_Ehdr *)buf;
    if (eh->e_ident[5] != ELFDATA2LSB ||
        (eh->e_type != ET_EXEC && eh->e_type != ET_DYN) ||
        !elf_machine_supported(eh->e_machine, ELFCLASS32) ||
        eh->e_phentsize < sizeof(Elf32_Phdr) ||
        eh->e_phnum == 0 || eh->e_phnum > MAX_PHDRS)
        return -ENOEXEC;
    return elf_load32(fd, eh, path, info);
}

/* ------------------------------------------------------------------ */
/*  Stack setup (argc, argv, envp, auxv)                              */
/* ------------------------------------------------------------------ */

#define AT_NULL     0
#define AT_PHDR     3
#define AT_PHENT    4
#define AT_PHNUM    5
#define AT_PAGESZ   6
#define AT_BASE     7
#define AT_FLAGS    8
#define AT_ENTRY    9
#define AT_UID      11
#define AT_EUID     12
#define AT_GID      13
#define AT_EGID     14
#define AT_PLATFORM 15
#define AT_HWCAP    16
#define AT_CLKTCK   17
#define AT_SECURE   23
#define AT_RANDOM   25
#define AT_HWCAP2   26

vaddr_t elf_setup_stack(vaddr_t stack_top, int argc, char *const argv[],
                        char *const envp[], const elf_load_info_t *info) {
    if (argc < 0 || argc > MAX_ARG_STRINGS)
        return 0;

    pt_root_t *pgdir = info->pgdir;
    vaddr_t sp_va  = stack_top;
    vaddr_t stack_bottom = stack_top - (uint64_t)USER_STACK_INITIAL_PAGES * PAGE_SIZE;

    int envc = 0;
    uintptr_t env_ptrs[MAX_ARG_STRINGS + 1];
    if (envp) {
        while (envc < MAX_ARG_STRINGS && envp[envc]) {
            int len = (int)strlen(envp[envc]) + 1;
            sp_va -= len;
            if (stack_copy(pgdir, sp_va, envp[envc], (size_t)len,
                           &stack_bottom) < 0)
                return 0;
            env_ptrs[envc] = (uintptr_t)sp_va;
            envc++;
        }
    }
    env_ptrs[envc] = 0;

    uintptr_t arg_ptrs[MAX_ARG_STRINGS + 1];
    for (int i = argc - 1; i >= 0; i--) {
        int len = (int)strlen(argv[i]) + 1;
        sp_va -= len;
        if (stack_copy(pgdir, sp_va, argv[i], (size_t)len,
                       &stack_bottom) < 0)
            return 0;
        arg_ptrs[i] = (uintptr_t)sp_va;
    }
    arg_ptrs[argc] = 0;

    sp_va &= ~15UL;

    const char *platform = ARCH_NAME;
    int plat_len = (int)strlen(platform) + 1;

    {
        size_t fixed = (size_t)plat_len +
                       (size_t)(envc + argc + 3) * sizeof(uintptr_t);
        sp_va -= (16 - (fixed & 15)) & 15;
    }

    sp_va -= plat_len;
    vaddr_t platform_va = sp_va;
    if (stack_copy(pgdir, sp_va, platform, (size_t)plat_len,
                   &stack_bottom) < 0)
        return 0;

    sp_va -= 16;
    vaddr_t random_va = sp_va;
    {
        unsigned char random_bytes[16];
        random_fill(random_bytes, sizeof(random_bytes));
        if (stack_copy(pgdir, sp_va, random_bytes, sizeof(random_bytes),
                       &stack_bottom) < 0)
            return 0;
    }

    uintptr_t auxv[][2] = {
        { AT_PHDR,   (uintptr_t)info->phdr_va  },
        { AT_PHENT,  (uintptr_t)info->phentsize },
        { AT_PHNUM,  (uintptr_t)info->phnum     },
        { AT_PAGESZ, (uintptr_t)PAGE_SIZE       },
        { AT_BASE,   (uintptr_t)info->interp_base },
        { AT_FLAGS,  0               },
        { AT_ENTRY,  (uintptr_t)info->exec_entry },
        { AT_UID,    0               },
        { AT_EUID,   0               },
        { AT_GID,    0               },
        { AT_EGID,   0               },
        { AT_PLATFORM, (uintptr_t)platform_va   },
        { AT_HWCAP,  0               },
        { AT_CLKTCK, 100             },
        { AT_SECURE, 0               },
        { AT_RANDOM, (uintptr_t)random_va       },
        { AT_HWCAP2, 0               },
        { AT_NULL,   0               },
    };
    int naux = (int)(sizeof(auxv) / sizeof(auxv[0]));

    sp_va -= naux * 2 * sizeof(uintptr_t);
    if (stack_copy(pgdir, sp_va, auxv,
                   (size_t)naux * 2 * sizeof(uintptr_t),
                   &stack_bottom) < 0)
        return 0;

    sp_va -= (envc + 1) * sizeof(uintptr_t);
    if (stack_copy(pgdir, sp_va, env_ptrs,
                   (size_t)(envc + 1) * sizeof(uintptr_t),
                   &stack_bottom) < 0)
        return 0;

    sp_va -= (argc + 1) * sizeof(uintptr_t);
    if (stack_copy(pgdir, sp_va, arg_ptrs,
                   (size_t)(argc + 1) * sizeof(uintptr_t),
                   &stack_bottom) < 0)
        return 0;

    /*
     * Linux process entry requires SP to satisfy the architecture ABI
     * alignment (16 bytes on all currently supported targets).  The pointer
     * vectors above are naturally word aligned but their total word count can
     * be odd, so align the final argc slot explicitly.
     */
    sp_va = (sp_va - sizeof(uintptr_t)) & ~15UL;
    {
        uintptr_t argc_value = (uintptr_t)argc;
        if (stack_copy(pgdir, sp_va, &argc_value, sizeof(argc_value),
                       &stack_bottom) < 0)
            return 0;
    }

    return sp_va;
}

#ifdef CONFIG_ABI_NATIVE
vaddr_t elf_setup_stack_a20(vaddr_t stack_top, int argc, char *const argv[],
                            char *const envp[], const elf_load_info_t *info,
                            uint32_t stdin_h, uint32_t stdout_h,
                            uint32_t stderr_h, uint32_t self_task_h,
                            uint32_t root_h, uint32_t cwd_h)
{
    if (argc < 0 || argc > MAX_ARG_STRINGS)
        return 0;

    pt_root_t *pgdir = info->pgdir;
    vaddr_t sp_va = stack_top;
    vaddr_t stack_bottom = stack_top - (uint64_t)USER_STACK_INITIAL_PAGES * PAGE_SIZE;

    int envc = 0;
    vaddr_t env_ptrs[MAX_ARG_STRINGS + 1];
    if (envp) {
        while (envc < MAX_ARG_STRINGS && envp[envc]) {
            int len = (int)strlen(envp[envc]) + 1;
            sp_va -= len;
            if (stack_copy(pgdir, sp_va, envp[envc], (size_t)len,
                           &stack_bottom) < 0)
                return 0;
            env_ptrs[envc] = sp_va;
            envc++;
        }
    }
    env_ptrs[envc] = 0;

    vaddr_t arg_ptrs[MAX_ARG_STRINGS + 1];
    for (int i = argc - 1; i >= 0; i--) {
        int len = (int)strlen(argv[i]) + 1;
        sp_va -= len;
        if (stack_copy(pgdir, sp_va, argv[i], (size_t)len,
                       &stack_bottom) < 0)
            return 0;
        arg_ptrs[i] = sp_va;
    }
    arg_ptrs[argc] = 0;

    sp_va &= ~15UL;

    sp_va -= (envc + 1) * sizeof(vaddr_t);
    if (stack_copy(pgdir, sp_va, env_ptrs,
                   (size_t)(envc + 1) * sizeof(vaddr_t),
                   &stack_bottom) < 0)
        return 0;
    vaddr_t envp_va = sp_va;

    sp_va -= (argc + 1) * sizeof(vaddr_t);
    if (stack_copy(pgdir, sp_va, arg_ptrs,
                   (size_t)(argc + 1) * sizeof(vaddr_t),
                   &stack_bottom) < 0)
        return 0;
    vaddr_t argv_va = sp_va;

    sp_va -= sizeof(a20_start_info_t);
    sp_va &= ~15UL;
    {
        a20_start_info_t si;
        memset(&si, 0, sizeof(si));
        si.size = sizeof(a20_start_info_t);
        si.version = 1;
        si.argc = (uint32_t)argc;
        si.envc = (uint32_t)envc;
        si.argv = argv_va;
        si.envp = envp_va;
        si.stdin_handle = stdin_h;
        si.stdout_handle = stdout_h;
        si.stderr_handle = stderr_h;
        si.self_task = self_task_h;
        si.root_dir = root_h;
        si.cwd_dir = cwd_h;
        si.page_size = PAGE_SIZE;
        if (stack_copy(pgdir, sp_va, &si, sizeof(si), &stack_bottom) < 0)
            return 0;
    }

    return sp_va;
}

/*
 * A dynamically linked native program has two consumers of its initial
 * stack: the ELF interpreter expects the conventional Linux layout, while
 * the A20 crt1 entry expects a20_start_info_t in a0.  Keep the native
 * descriptor in the reserved top of the stack and return the conventional
 * stack pointer to the interpreter.
 */
vaddr_t elf_setup_stack_a20_dynamic(vaddr_t stack_top, int argc,
                                    char *const argv[], char *const envp[],
                                    const elf_load_info_t *info,
                                    uint32_t stdin_h, uint32_t stdout_h,
                                    uint32_t stderr_h, uint32_t self_task_h,
                                    uint32_t root_h, uint32_t cwd_h,
                                    vaddr_t *start_info_out)
{
    if (!start_info_out)
        return 0;

    vaddr_t si_va = stack_top - sizeof(a20_start_info_t);
    vaddr_t sp_va = elf_setup_stack(si_va, argc, argv, envp, info);
    if (!sp_va)
        return 0;

    int envc = 0;
    if (envp) {
        while (envc < MAX_ARG_STRINGS && envp[envc])
            envc++;
    }

    a20_start_info_t si;
    memset(&si, 0, sizeof(si));
    si.size = sizeof(si);
    si.version = 1;
    si.argc = (uint32_t)argc;
    si.envc = (uint32_t)envc;
    si.argv = sp_va + sizeof(vaddr_t);
    si.envp = si.argv + (uint64_t)(argc + 1) * sizeof(vaddr_t);
    si.stdin_handle = stdin_h;
    si.stdout_handle = stdout_h;
    si.stderr_handle = stderr_h;
    si.self_task = self_task_h;
    si.root_dir = root_h;
    si.cwd_dir = cwd_h;
    si.page_size = PAGE_SIZE;

    vaddr_t stack_bottom = stack_top -
        (uint64_t)USER_STACK_INITIAL_PAGES * PAGE_SIZE;
    if (stack_copy(info->pgdir, si_va, &si, sizeof(si), &stack_bottom) < 0)
        return 0;

    *start_info_out = si_va;
    return sp_va;
}
#endif

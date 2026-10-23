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
#ifdef CONFIG_NOMMU
#include "mm/elf_nommu_allocs.h"
#endif
#include "mm/mm.h"
#include "mm/vm.h"
#include "vm_internal.h"
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
#include "proc/proc.h"
#include "mm/elf_rust.h"
#include "mm/frame.h"
#include "mm/pt.h"
#include "drivers/driver_descriptor.h"
#include <stdint.h>
#if defined(CONFIG_ABI_NATIVE) || defined(CONFIG_ABI_BOTH)
#include "ipc/start_info.h"
#endif

/* musl struct pthread is ~300-400 bytes; 512 gives headroom */

#define ASLR_BITS          11
#define ASLR_MASK          (((1UL << ASLR_BITS) - 1) << 16)

/* Max program headers we read in one shot (3584 bytes on stack) */
#define MAX_PHDRS          64

#ifdef CONFIG_NOMMU
static void elf_transfer_nommu_allocs(mm_struct_t *mm, elf_load_info_t *info) {
    elf_nommu_allocs_transfer(info->nommu_allocs, info->nommu_alloc_sizes,
                              info->nommu_alloc_types, &info->num_nommu_allocs,
                              mm->nommu_allocs, mm->nommu_alloc_sizes,
                              mm->nommu_alloc_types, &mm->num_nommu_allocs,
                              NOMMU_ALLOC_MAX);
}

static void elf_release_nommu_alloc(void *ptr) { kfree(ptr); }

static void elf_discard_nommu_allocs(mm_struct_t *mm) {
    elf_nommu_allocs_release(mm->nommu_allocs, mm->nommu_alloc_sizes,
                             mm->nommu_alloc_types, &mm->num_nommu_allocs,
                             NOMMU_ALLOC_MAX, elf_release_nommu_alloc);
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

#ifndef CONFIG_NOMMU
static uint64_t elf_aslr_bias(void) {
    uint64_t r = random_u64();
    uint64_t bits = (r ^ (r >> 29) ^ (r >> 47)) & ((1UL << ASLR_BITS) - 1);
    return bits << 16;
}
#endif

static int elf_path_has_driver_suffix(const char *path)
{
    size_t len = path ? strlen(path) : 0;
    return len >= 7 && strcmp(path + len - 7, ".a20drv") == 0;
}

static int elf_validate_user_driver(int fd, const Elf64_Ehdr *eh)
{
    if (eh->e_shentsize != sizeof(Elf64_Shdr) || !eh->e_shnum ||
        eh->e_shnum > 128 || eh->e_shstrndx >= eh->e_shnum)
        return -ENOEXEC;

    Elf64_Shdr shdrs[128];
    size_t shdr_bytes = (size_t)eh->e_shnum * sizeof(Elf64_Shdr);
    if (vfs_pread(fd, (char *)shdrs, shdr_bytes, eh->e_shoff) !=
        (int)shdr_bytes)
        return -ENOEXEC;

    Elf64_Shdr *shstr = &shdrs[eh->e_shstrndx];
    if (!shstr->sh_size || shstr->sh_size > 4096)
        return -ENOEXEC;
    char names[4096];
    if (vfs_pread(fd, names, shstr->sh_size, shstr->sh_offset) !=
        (int)shstr->sh_size)
        return -ENOEXEC;

    for (uint16_t i = 0; i < eh->e_shnum; i++) {
        Elf64_Shdr *sh = &shdrs[i];
        if (sh->sh_name >= shstr->sh_size)
            continue;
        const char *section_name = names + sh->sh_name;
        size_t name_left = shstr->sh_size - sh->sh_name;
        size_t name_len = 0;
        while (name_len < name_left && section_name[name_len])
            name_len++;
        if (name_len == name_left || name_len != 7 ||
            memcmp(section_name, ".a20drv", 7) != 0)
            continue;
        a20_driver_descriptor_t desc;
        if (sh->sh_size != sizeof(desc) ||
            vfs_pread(fd, (char *)&desc, sizeof(desc), sh->sh_offset) !=
                (int)sizeof(desc))
            return -ENOEXEC;
        return a20_driver_descriptor_sane(&desc) &&
               desc.placement == A20_DRIVER_PLACEMENT_USER_SERVICE &&
               desc.type >= A20_DRIVER_TYPE_RTC &&
               desc.type <= A20_DRIVER_TYPE_SECURITY && desc.name[0] ?
               0 : -ENOEXEC;
    }
    return -ENOEXEC;
}

#ifndef CONFIG_NOMMU
static uint64_t pte_to_vm_flags(pte_t pte_flags) {
    return VM_ANON | mm_pte_flags_to_vm_flags(pte_flags);
}
#endif

static int elf_add_vma(mm_struct_t *mm, vaddr_t start, vaddr_t end,
                       uint64_t vm_flags, pte_t pte_flags, bool anon) {
    if (!mm) return 0;
    mm_seg_t *vma = mm_seg_new();
    if (!vma) return -ENOMEM;
    vma->start     = start;
    vma->end       = end;
    vma->vm_flags  = vm_flags;
    vma->pte_flags = pte_flags;
    vma->file      = NULL;
    uint64_t flags = spin_lock_irqsave(&mm->lock);
    mm_insert_vma(mm, vma);
    mm->total_vm += (end - start) / PAGE_SIZE;
#if defined(ARCH_HAS_PGTABLE_OPS) && !defined(CONFIG_NOMMU)
    /* `anon` cannot be inferred from vm_flags: pte_to_vm_flags() stamps
     * VM_ANON onto *every* file-backed segment, so a private RELRO segment
     * arrives looking exactly like .bss (VM_ANON set, no VM_FILE bit -- its
     * file_fd is only assigned after this returns).  Provisioning such a
     * range made the fault path hand out anonymous zero pages instead of
     * file content and killed exec with SIGSEGV, so the call site decides. */
    if (anon && !(vm_flags & VM_SHARED))
        (void)mm_pt_provision_anon(mm, start, end, pte_flags);
#else
    (void)anon;
#endif
    spin_unlock_irqrestore(&mm->lock, flags);
    mm_vma_flush_deferred(mm);
    return 0;
}

static void elf_discard_vmas(mm_struct_t *mm)
{
    mm_seg_t *vma = mm->mmap;
    while (vma) {
        mm_seg_t *next = vma->next;
        /*
         * Release the VMA's mapped pages under the same ownership rule
         * mm_destroy() uses.  This matters because vdso_map_image() maps the
         * global vDSO/vvar frames (VM_PFNMAP, shared, never freed) into the
         * image page table: reaping those leaves with frame_put would drop a
         * permanently-owned frame onto the buddy list and corrupt it.  A plain
         * anonymous leaf (segment, stack, TLS) still returns its frame here,
         * and a file-backed leaf drops its page-cache pin instead.
         */
        free_vma_pages(mm, vma);
        if ((vma->vm_flags & VM_FILE) && vma->file) {
            if (vma->file_vnode) {
                if (vma->vm_flags & VM_SHARED)
                    vnode_shared_map_dec(vma->file_vnode);
                vnode_put(vma->file_vnode);
            }
            vfs_put_file(vma->file);
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
    mm_struct_t mm;
    memset(&mm, 0, sizeof(mm));
    mm.mmap = info->mmap;
    mm.pgdir = info->pgdir;
    spin_init(&mm.lock);
    elf_discard_vmas(&mm);
    /* free_vma_pages() may have retired now-empty table pages; reclaim them
     * before the root goes away (needs neither mm->lock nor a live mm). */
    mm_pt_retire_drain(&mm);
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
 * Make sure the page containing sp_va is mapped in the page table.
 * If sp_va lies below the current stack bottom, allocate a new physical page
 * and map it.  This stops execve from overflowing the stack when argv/envp are
 * large: the write to an unmapped address would leave a stale page-table entry
 * that is then used as a physical address and crashes.
 *
 * @pgdir         page table root pointer
 * @sp_va         the virtual address to make accessible
 * @stack_bottom  current stack bottom (passed as a pointer, may be updated)
 * @max_grow      how many pages the stack may grow downwards at most
 * @return        the mapped physical address (kernel direct map), or NULL on
 *                failure
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

typedef struct {
    pt_root_t *pgdir;
    vaddr_t stack_bottom;
} elf_stack_writer_t;

static int32_t elf_stack_copy_ffi(void *opaque, uint64_t dst_va,
                                  const uint8_t *src, size_t len)
{
    elf_stack_writer_t *writer = opaque;
    return stack_copy(writer->pgdir, (vaddr_t)dst_va, src, len,
                      &writer->stack_bottom);
}

static int32_t elf_stack_random_ffi(void *opaque, uint8_t *dst, size_t len)
{
    (void)opaque;
    random_fill(dst, len);
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

#if !defined(CONFIG_NOMMU) && !defined(CONFIG_ELF_EAGER_LOAD)
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
    for (mm_seg_t *vma = mm->mmap; vma; vma = vma->next) {
        if (vma->start < end && vma->end > start)
            return -EINVAL;
    }

    if (file_map_end > start) {
        vaddr_t mapped = mm_mmap_file(mm, start, file_map_end - start,
                                      prot, MAP_PRIVATE | MAP_FIXED, fd,
                                      file_page_offset);
        if (mm_addr_is_error(mapped))
            return mm_addr_error(mapped);
        /*
         * ppc64le pSeries delivers instruction storage faults to the external
         * vector with SRR0 clobbered to the trap-entry address, so a code page
         * fault cannot recover the faulting EA and demand paging never maps the
         * page.  Map executable file pages eagerly to avoid code-fetch faults.
         */
#if ARCH_INSN_FAULT_UNRECOVERABLE
        if (flags & PTE_X) {
            for (vaddr_t page = start; page < file_map_end; page += PAGE_SIZE) {
                pte_t *pte = pt_lookup_leaf(pgdir, page, NULL, NULL, NULL);
                if (pte && (*pte & PTE_V))
                    continue;
                void *frame = frame_alloc();
                if (!frame)
                    return -ENOMEM;
                memset(frame, 0, PAGE_SIZE);
                int nr = vfs_pread(fd, (char *)frame, PAGE_SIZE,
                                   file_page_offset + (page - start));
                if (nr < 0) {
                    frame_free(frame);
                    return nr;
                }
                int r = pt_map(pgdir, page, va_to_pa(frame), flags);
                if (r < 0) {
                    frame_free(frame);
                    return r;
                }
                if (flags & PTE_X)
                    arch_flush_icache_range(frame, PAGE_SIZE);
                mm_rss_add(mm, 1);
            }
        }
#endif
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

        /* Head partial page: bytes preceding file_offset are part of this
         * page too (ELF header + program headers).  Back them so AT_PHDR
         * resolves for dynamically linked natives (08-runtime-status §8a). */
        if (copy_off > 0) {
            int hr = vfs_pread(fd, (char *)frame, copy_off,
                               file_page_offset);
            (void)hr; /* short header read simply leaves zeros behind */
        }

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
                        pte_to_vm_flags(flags), flags, false);
        if (r < 0)
            return r;
        mm_rss_add(mm, 1);
        anon_start = page + PAGE_SIZE;
    } else {
        anon_start = file_end;
    }

    anon_start = ROUND_UP(anon_start, PAGE_SIZE);
    if (anon_start < end)
        return elf_add_vma(mm, anon_start, end,
                           pte_to_vm_flags(flags), flags, false);
    return 0;
}
#endif

#ifdef CONFIG_NOMMU
/*
 * NOMMU static-pie relocation.
 *
 * The whole apply loop — the dynamic-tag scan, the hash/symbol bound, the
 * reltype selection, the weak/undefined policy, the destination-range and
 * permission checks, and the stores themselves — lives in the Rust core
 * (a20_elf_apply_relocs). The adapter hands over the flat loaded image and the
 * plan it executed; it performs no pointer arithmetic on values that come from
 * the image.
 */
static int elf_apply_relocs(const a20_elf_image_plan_t *plan, uint64_t load_bias,
                            uint8_t *image, uint64_t image_len, bool elf64) {
    int64_t applied = a20_elf_apply_relocs(plan, load_bias, image,
                                           (size_t)image_len, elf64);
    if (applied < 0)
        return (int)applied;
    if (applied)
        ktrace_mm("[ELF] reloc: %lld entr%s, bias 0x%lx\n",
                  (long long)applied, applied == 1 ? "y" : "ies",
                  (unsigned long)load_bias);
    return 0;
}

#else /* !CONFIG_NOMMU */
static int elf_apply_relocs(const a20_elf_image_plan_t *plan, uint64_t load_bias,
                            uint8_t *image, uint64_t image_len, bool elf64) {
    (void)plan; (void)load_bias; (void)image; (void)image_len; (void)elf64;
    return 0;
}
#endif /* CONFIG_NOMMU */

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
#ifdef CONFIG_NOMMU
    (void)mm; (void)pgdir; (void)flags;
    /* In NOMMU, va is the physical address. We assume the system/loader 
     * has ensured this memory is available for this segment. */
    if (src->kind == SEG_BUF) {
        memcpy((void *)(uintptr_t)va, src->buf.data, filesz);
    } else {
        int nr = vfs_pread(src->fd.fd, (void *)(uintptr_t)va, filesz,
                           src->fd.offset);
        if (nr != (int)filesz)
            kerr("[ELF] short read: %d/%lu\n", nr, (unsigned long)filesz);
        if (nr < 0) return nr;
    }
    if (memsz > filesz) {
        memset((void *)(uintptr_t)(va + filesz), 0, memsz - filesz);
    }
    return 0;
#else
    vaddr_t start = va & ~(vaddr_t)(PAGE_SIZE - 1);
    vaddr_t end   = ROUND_UP(va + memsz, PAGE_SIZE);
    char tmp[PAGE_SIZE];

    if (src->kind == SEG_FD) {
#ifndef CONFIG_ELF_EAGER_LOAD
        int r = map_fd_segment_lazy(mm, pgdir, va, memsz, src->fd.fd,
                                    (uint64_t)src->fd.offset, filesz, flags);
        if (r != -EINVAL)
            return r;
        /* Non-conforming offset/vaddr alignment retains the eager fallback. */
#endif
    }

    for (vaddr_t page = start; page < end; page += PAGE_SIZE) {
        void *frame = frame_alloc();
        if (!frame) return -ENOMEM;

        /* Preserve existing page contents if page already mapped */
        pte_t *old_pte = pt_walk(pgdir, page, 0);
        int page_already_mapped = old_pte && (*old_pte & PTE_V);
        if (page_already_mapped) {
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

        /* Head partial page: file bytes preceding p_offset are part of
         * this page too (ELF header + program headers).  Back them so
         * AT_PHDR resolves for dynamically linked natives
         * (docs/status/current/native-abi-runtime-status.md §8a).
         * Only when no earlier PT_LOAD already backed this page: the bytes
         * [page, va) then belong to that earlier segment, and overlaying
         * this segment's preceding file bytes would clobber them (a
         * filesz==0 BSS LOAD whose p_offset does not continue the previous
         * segment's file range would rewrite live .data with ELF header
         * bytes). */
        if (page < va && src->kind == SEG_FD && !page_already_mapped) {
            uint64_t foff = (uint64_t)src->fd.offset;
            if (foff > 0) {
                uint64_t back = va - page;
                uint64_t have = foff < back ? foff : back;
                int nr = vfs_pread(src->fd.fd, tmp, (size_t)have,
                                   foff - have);
                if (nr > 0)
                    memcpy((char *)frame, tmp, (size_t)nr);
            }
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
    /* The exec image is not active on any CPU until exec_install_process(). */
    return elf_add_vma(mm, start, end, pte_to_vm_flags(flags), flags, false);
#endif
}

/* ------------------------------------------------------------------ */
/*  Stack mapping                                                     */
/* ------------------------------------------------------------------ */

static int map_stack(mm_struct_t *mm, pt_root_t *pgdir, vaddr_t *stack_top_out) {
#ifdef CONFIG_NOMMU
    (void)pgdir;
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
                       mm_user_stack_pte_flags(), true);
#else
    /* ASLR: the stack top is offset downwards at page granularity inside the
     * [USER_STACK_FLOOR + initial stack, USER_STACK_TOP + PAGE) window.  Below
     * that window are the fixed vDSO/vvar and TLS regions. */
    vaddr_t stack_top    = USER_STACK_TOP + PAGE_SIZE - mm_aslr_stack_offset();
    vaddr_t stack_bottom = stack_top -
        (uint64_t)USER_STACK_INITIAL_PAGES * PAGE_SIZE;

    for (int i = 0; i < USER_STACK_INITIAL_PAGES; i++) {
        vaddr_t va = stack_top -
                      (uint64_t)(USER_STACK_INITIAL_PAGES - i) * PAGE_SIZE;
        void *frame = frame_alloc();
        if (!frame) return -ENOMEM;
        memset(frame, 0, PAGE_SIZE);
        int r = pt_map(pgdir, va, va_to_pa(frame),
                       mm_user_stack_pte_flags());
        if (r < 0) { frame_free(frame); return r; }
    }
    *stack_top_out = stack_top;
    return elf_add_vma(mm, stack_bottom, stack_top,
                       VM_ANON | VM_READ | VM_WRITE | VM_STACK,
                       mm_user_stack_pte_flags(), true);
#endif
}

/* ------------------------------------------------------------------ */
/*  TLS setup                                                         */
/* ------------------------------------------------------------------ */

static int setup_tls(mm_struct_t *mm, pt_root_t *pgdir,
                     const void *tls_data, uint64_t tls_filesz,
                     uint64_t tcb_offset, uint64_t total_size,
                     uint64_t total_pages, uint64_t tcb_size,
                     vaddr_t *tls_va_out, vaddr_t *tls_tp_out) {
    (void)total_size;
    pte_t pte_flags = mm_user_brk_pte_flags();

#ifdef CONFIG_NOMMU
    (void)pgdir; (void)pte_flags; (void)tcb_size;
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
    memset(tcb_dst, 0, tcb_size);
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
    return elf_add_vma(mm, tls_va,
                       tls_va + total_pages * PAGE_SIZE,
                       VM_ANON | VM_READ | VM_WRITE, pte_flags, true);
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
/* ------------------------------------------------------------------ */
/*  Rust-decision loader                                                */
/*                                                                    */
/*  The business decisions of elf_load64/32/from_buf live in           */
/*  kernel/mm/elf_core.rs: header/phdr validation, wrap-safe bounds,   */
/*  segment permission/W^X/layout planning, load bias, TLS/INTERP/     */
/*  DYNAMIC discovery, native-ABI verdict, foreign probe and the       */
/*  initial-stack plan. This file keeps only the adapter: reading the  */
/*  bytes, mapping the planned segments, and the NOMMU relocation      */
/*  memory writes. Nothing here re-derives a decision the plan made.   */
/* ------------------------------------------------------------------ */

#define A20_ELF_EHDR32_SIZE ((size_t)sizeof(Elf32_Ehdr))

static int elf_native_abi_available(int is_native, const char *path);

typedef struct {
    int         fd;        /* fd source, or -1 */
    const void *buf;       /* buffer source, or NULL */
    size_t      buf_len;
} elf_rust_src_t;

/* Copy exactly len bytes at file offset @off. Buffer sources must contain
 * the whole extent; short inputs fail with -ENOEXEC instead of copying bytes
 * beyond the supplied buffer. The parser fixture set documents this boundary. */
static int elf_rust_pread(const elf_rust_src_t *src, void *dst, size_t len,
                          uint64_t off)
{
    if (src->buf) {
        if (off > src->buf_len || len > src->buf_len - (size_t)off)
            return -ENOEXEC;
        memcpy(dst, (const char *)src->buf + (size_t)off, len);
        return 0;
    }
    int r = vfs_pread(src->fd, dst, len, (long)off);
    if (r < 0)
        return r;
    return (size_t)r == len ? 0 : -ENOEXEC;
}

/* Read up to @want bytes at file offset @off; *got receives the byte
 * count. The buffer source is pre-sliced by the caller, so @off applies to
 * fd sources only. */
static int elf_rust_read_up_to(const elf_rust_src_t *src, void *dst,
                               size_t want, uint64_t off, size_t *got)
{
    if (src->buf) {
        size_t n = want < src->buf_len ? want : src->buf_len;
        memcpy(dst, src->buf, n);
        *got = n;
        return 0;
    }
    int r = vfs_pread(src->fd, dst, want, (long)off);
    if (r < 0)
        return r;
    *got = (size_t)r;
    return 0;
}

/* The one adapter-supplied W^X fact: the per-process translator exemption.
 * The policy value itself comes from mm_wx_policy(); both are inputs to the
 * Rust filter, which owns the verdict. */
static bool elf_rust_xlator_host(void)
{
#ifdef CONFIG_XLATOR
    task_t *cur = proc_current();
    return cur && cur->xlator_host;
#else
    return false;
#endif
}

typedef struct {
    const elf_rust_src_t *src;
    mm_struct_t *mm;
    pt_root_t *pgdir;
} elf_segment_map_context_t;

typedef struct {
    const elf_rust_src_t *src;
    const char *path;
    elf_load_info_t *info;
    const a20_elf_image_plan_t *plan;
    mm_struct_t mm;
    elf_segment_map_context_t map;
    a20_elf_layout_t layout;
    uint64_t span_min, span_max;
    a20_elf_load_resources_t resources;
} elf_load_transaction_t;

static int32_t elf_rust_map_segment_ffi(
    void *opaque, const a20_elf_segment_plan_t *segment, uint64_t load_bias);

static int32_t elf_load_tx_begin(void *opaque, uint64_t min, uint64_t max,
                                 uint64_t *bias_out)
{
    elf_load_transaction_t *tx = opaque;
    if (!tx || !bias_out || max <= min) return -EINVAL;
    tx->span_min = min;
    tx->span_max = max;
    pt_root_t *pgdir = pt_create();
    if (!pgdir) return -ENOMEM;
    pt_map_kernel(pgdir);
    tx->mm = (mm_struct_t){ .pgdir = pgdir, .mmap_base = MMAP_BASE_ADDR };
#ifdef CONFIG_NOMMU
    vaddr_t alloc_bias = 0;
    int rc = elf_alloc_nommu_image(&tx->mm, (vaddr_t)min, (vaddr_t)max,
                                   &alloc_bias);
    if (rc < 0) {
        elf_discard_nommu_allocs(&tx->mm);
        pt_destroy_user(pgdir); tx->mm.pgdir = NULL;
        return rc;
    }
    *bias_out = alloc_bias;
#else
    *bias_out = a20_elf_plan_load_bias(tx->plan->is_dyn, USER_DYN_BASE,
                                       elf_aslr_bias());
#endif
    tx->resources.pgdir = (uint64_t)(uintptr_t)pgdir;
    tx->resources.mm = (uint64_t)(uintptr_t)&tx->mm;
    tx->map = (elf_segment_map_context_t){ tx->src, &tx->mm, pgdir };
    return 0;
}

static int32_t elf_load_tx_map(void *opaque, uint64_t bias)
{
    elf_load_transaction_t *tx = opaque;
    return a20_elf_map_planned_segments(tx->plan, bias, &tx->map,
                                        elf_rust_map_segment_ffi);
}

static int32_t elf_load_tx_stage_tls(void *opaque)
{
    elf_load_transaction_t *tx = opaque;
    a20_elf_load_resources_t *res = &tx->resources;
    if (!res->tls_filesz) return 0;
    void *buffer = kmalloc((size_t)res->tls_filesz);
    if (!buffer) return -ENOMEM;
    res->tls_buffer = (uint64_t)(uintptr_t)buffer;
    return elf_rust_pread(tx->src, buffer, (size_t)res->tls_filesz,
                          res->tls_offset);
}

static int32_t elf_load_tx_relocate(void *opaque, uint64_t bias, uint64_t span)
{
    elf_load_transaction_t *tx = opaque;
    return elf_apply_relocs(tx->plan, bias,
                            (uint8_t *)(uintptr_t)(bias + tx->span_min),
                            span, tx->plan->ehdr.class == ELFCLASS64);
}

static int32_t elf_load_tx_finish_layout(void *opaque, const a20_elf_layout_t *layout)
{
    elf_load_transaction_t *tx = opaque;
    if (!layout) return -EINVAL;
    tx->layout = *layout;
    tx->resources.mmap = (uint64_t)(uintptr_t)tx->mm.mmap;
#ifdef CONFIG_NOMMU
    if (tx->layout.max_va > tx->layout.base) {
        int rc = elf_add_vma(&tx->mm, (vaddr_t)tx->layout.base,
                             (vaddr_t)tx->layout.max_va,
                             VM_ANON | VM_READ | VM_WRITE | VM_EXEC,
                             mm_user_brk_pte_flags(), true);
        if (rc < 0) return rc;
    }
#endif
    return 0;
}

static int32_t elf_load_tx_read_interpreter(void *opaque, uint64_t offset,
                                             uint8_t *dst, size_t len)
{
    elf_load_transaction_t *tx = opaque;
    return elf_rust_pread(tx->src, dst, len, offset);
}

static int32_t elf_load_tx_open_interpreter(void *opaque, const uint8_t *path,
                                              size_t len, int32_t *fd_out)
{
    elf_load_transaction_t *tx = opaque;
    if (!path || !fd_out || len >= MAX_PATH_LEN) return -EINVAL;
    int fd = vfs_open((const char *)path, O_RDONLY, 0);
    if (fd < 0) return fd;
    tx->resources.interp_fd = fd;
    *fd_out = fd;
    return 0;
}

static int32_t elf_load_tx_read_interp_image(void *opaque, uint64_t off,
                                              uint8_t *dst, size_t want,
                                              size_t *got)
{
    elf_load_transaction_t *tx = opaque;
    elf_rust_src_t src = { .fd = tx->resources.interp_fd, .buf = NULL, .buf_len = 0 };
    return elf_rust_read_up_to(&src, dst, want, off, got);
}

static int32_t elf_load_tx_interpreter_bias(void *opaque, uint64_t min,
                                             uint64_t max, uint64_t *out)
{
    elf_load_transaction_t *tx = opaque;
    if (!out || max <= min) return -EINVAL;
#ifdef CONFIG_NOMMU
    return elf_alloc_nommu_image(&tx->mm, (vaddr_t)min, (vaddr_t)max,
                                 (vaddr_t *)out);
#else
    (void)tx;
    *out = INTERP_BASE_ADDR + elf_aslr_bias();
    return 0;
#endif
}

static int32_t elf_load_tx_map_interp_segment(void *opaque,
                                               const a20_elf_segment_plan_t *seg,
                                               uint64_t bias)
{
    elf_load_transaction_t *tx = opaque;
    if (!seg) return -EINVAL;
    seg_src_t src = seg_from_fd(tx->resources.interp_fd, (long)seg->offset);
    return map_segment(&tx->mm, (pt_root_t *)(uintptr_t)tx->resources.pgdir,
                       (vaddr_t)(seg->va + bias), seg->memsz,
                       &src, seg->filesz, mm_prot_to_pte_flags(seg->prot));
}

static int32_t elf_load_tx_finish_interp(void *opaque,
                                           const a20_elf_layout_t *layout)
{
    elf_load_transaction_t *tx = opaque;
    if (!layout) return -EINVAL;
#ifdef CONFIG_NOMMU
    if (layout->max_va > layout->base)
        return elf_add_vma(&tx->mm, (vaddr_t)layout->base,
                           (vaddr_t)layout->max_va,
                           VM_ANON | VM_READ | VM_WRITE | VM_EXEC,
                           mm_user_brk_pte_flags(), true);
#else
    (void)tx;
#endif
    return 0;
}

static int32_t elf_load_tx_build_interp_path(void *opaque, uint32_t kind,
                                             const uint8_t *interp,
                                             uint8_t *out, size_t cap)
{
    elf_load_transaction_t *tx = opaque;
    if (!tx || !interp || !out || cap == 0) return -EINVAL;
    switch (kind) {
    case 1: return path_build_mount_relative(tx->path, (const char *)interp, (char *)out, cap);
    case 2: return path_build_sibling(tx->path, (const char *)interp, (char *)out, cap);
    case 3: return path_build_sibling(tx->path, "/lib/libc.so", (char *)out, cap);
    default: return -EINVAL;
    }
}

static int32_t elf_load_tx_arch_interp_path(void *opaque, const uint8_t *interp,
                                             uint8_t *out, size_t cap)
{
    elf_load_transaction_t *tx = opaque;
    if (!tx || !interp || !out || cap == 0) return -EINVAL;
    return arch_resolve_interp_fallback(tx->path, (const char *)interp,
                                        (char *)out, cap);
}

static int32_t elf_load_tx_interpreter(void *opaque, uint64_t bias)
{
    elf_load_transaction_t *tx = opaque;
    (void)bias;
    a20_elf_interpreter_ops_t ops = {
        .context = tx,
        .read = elf_load_tx_read_interp_image,
        .choose_bias = elf_load_tx_interpreter_bias,
        .map_segment = elf_load_tx_map_interp_segment,
        .finish = elf_load_tx_finish_interp,
        .expect_class = ARCH_ELF_CLASS,
        .wx_xlator_host = elf_rust_xlator_host(),
        .expect_machine = ARCH_ELF_MACHINE,
        .wx_policy = mm_wx_policy(),
#ifdef CONFIG_NOMMU
        .nommu = 1,
#endif
        ._pad1 = {0},
    };
    return a20_elf_load_interpreter(&ops, &tx->resources.interp_entry,
                                    &tx->resources.interp_base);
}

static void elf_load_tx_close_interpreter(void *opaque)
{
    elf_load_transaction_t *tx = opaque;
    if (tx->resources.interp_fd >= 0) vfs_close(tx->resources.interp_fd);
}

static int32_t elf_load_tx_stack(void *opaque)
{
    elf_load_transaction_t *tx = opaque;
    return map_stack(&tx->mm, (pt_root_t *)(uintptr_t)tx->resources.pgdir,
                     (vaddr_t *)&tx->resources.stack_top);
}

static int32_t elf_load_tx_install_tls(void *opaque)
{
    elf_load_transaction_t *tx = opaque;
    int rc = setup_tls(&tx->mm, (pt_root_t *)(uintptr_t)tx->resources.pgdir,
                       (void *)(uintptr_t)tx->resources.tls_buffer,
                       tx->resources.tls_filesz, tx->resources.tls_tcb_offset,
                       tx->resources.tls_total_size, tx->resources.tls_pages,
                       tx->resources.tls_tcb_size,
                       (vaddr_t *)&tx->resources.tls_va,
                       (vaddr_t *)&tx->resources.tls_tp);
    return rc;
}

static void elf_load_tx_free_tls(void *opaque)
{
    elf_load_transaction_t *tx = opaque;
    kfree((void *)(uintptr_t)tx->resources.tls_buffer);
}

static void elf_load_tx_discard_vmas(void *opaque)
{ elf_discard_vmas(&((elf_load_transaction_t *)opaque)->mm); }
static void elf_load_tx_discard_nommu(void *opaque)
{
#ifdef CONFIG_NOMMU
    elf_discard_nommu_allocs(&((elf_load_transaction_t *)opaque)->mm);
#else
    (void)opaque;
#endif
}
static void elf_load_tx_destroy_pgdir(void *opaque)
{
    elf_load_transaction_t *tx = opaque;
    pt_destroy_user((pt_root_t *)(uintptr_t)tx->resources.pgdir);
}

static int32_t elf_load_tx_brk_offset(void *opaque, uint64_t base, uint64_t *out)
{
    (void)opaque;
    if (!out) return -EINVAL;
    *out = mm_aslr_brk_offset((vaddr_t)base);
    return 0;
}

static int32_t elf_load_tx_commit(void *opaque, const a20_elf_load_result_t *r)
{
    elf_load_transaction_t *tx = opaque;
    if (!r) return -EINVAL;
    *tx->info = (elf_load_info_t){
        .entry = (uintptr_t)r->entry, .exec_entry = (uintptr_t)r->exec_entry,
        .base = (vaddr_t)r->base, .end_va = (vaddr_t)r->end_va,
        .brk = (vaddr_t)r->brk, .phdr_va = (vaddr_t)r->phdr_va,
        .phnum = r->phnum, .phentsize = r->phentsize,
        .load_addr = (vaddr_t)r->load_addr, .load_size = (size_t)r->load_size,
        .pgdir = (pt_root_t *)(uintptr_t)r->pgdir, .stack_top = (vaddr_t)r->stack_top,
        .tls_va = (vaddr_t)r->tls_va, .tls_size = r->tls_size,
        .tls_tp = (vaddr_t)r->tls_tp, .interp_base = (vaddr_t)r->interp_base,
        .mmap = (struct mm_seg *)(uintptr_t)r->mmap,
        .is_native_abi = r->is_native_abi,
        .path = tx->path,
    };
#ifdef CONFIG_NOMMU
    elf_transfer_nommu_allocs(&tx->mm, tx->info);
#endif
    return 0;
}

/* Mechanism for one already-validated PT_LOAD plan entry. Rust chooses the
 * iteration order and stops at the first failed segment; C performs only the
 * source-specific page mapping. */
static int32_t elf_rust_map_segment_ffi(
    void *opaque, const a20_elf_segment_plan_t *segment, uint64_t load_bias)
{
    elf_segment_map_context_t *ctx = opaque;
    if (!ctx || !ctx->src || !segment)
        return -EFAULT;
    vaddr_t seg_va = (vaddr_t)(segment->va + load_bias);
    pte_t flags = mm_prot_to_pte_flags(segment->prot);
    if (ctx->src->buf) {
        if (segment->offset > ctx->src->buf_len ||
            segment->filesz > ctx->src->buf_len - (size_t)segment->offset)
            return -ENOEXEC;
        seg_src_t ss = seg_from_buf((const char *)ctx->src->buf + segment->offset);
        return map_segment(ctx->mm, ctx->pgdir, seg_va, segment->memsz,
                           &ss, segment->filesz, flags);
    }
    seg_src_t ss = seg_from_fd(ctx->src->fd, (long)segment->offset);
    return map_segment(ctx->mm, ctx->pgdir, seg_va, segment->memsz,
                       &ss, segment->filesz, flags);
}

/* Plan an image out of the ehdr/phdr bytes and execute the plan: map every
 * PT_LOAD through the adapter, set up TLS, run the NOMMU static-pie
 * relocations, and (when PT_INTERP is present) resolve and load the
 * interpreter. Fills *info like elf_load64/32 did. */
#if defined(__GNUC__) && !defined(__clang__)
/* GCC's -fsanitize=undefined instrumentation makes -Wmaybe-uninitialized
 * fire a false positive on the tls_data ownership web below (it is nulled
 * before every failure path). Silence just this function. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
static int elf_rust_load_planned(const elf_rust_src_t *src, const char *path,
                                 elf_load_info_t *info)
{
    unsigned char ehdr_buf[sizeof(Elf64_Ehdr)];
    size_t ehdr_got = 0;
    int r = elf_rust_read_up_to(src, ehdr_buf, sizeof(ehdr_buf), 0, &ehdr_got);
    if (r < 0)
        return r;
    /* Mirror the C gate: fewer bytes than a 32-bit header is not an image.
     * A 52..63-byte tail is rejected deterministically where the C path
     * read an uninitialised stack tail (fixture f10, OBSERVE row). */
    if (ehdr_got < A20_ELF_EHDR32_SIZE)
        return -ENOEXEC;
    if (*(uint32_t *)ehdr_buf != ELF_MAGIC)
        return -ENOEXEC;
    uint8_t class = ehdr_buf[4];
    if (class != ELFCLASS32 && class != ELFCLASS64)
        return -ENOEXEC;

    a20_elf_ehdr_t eh;
    r = a20_elf_parse_header(ehdr_buf, ehdr_got, &eh);
    if (r < 0)
        return r;

    /* Read the program-header table. The plan re-validates count and entry
     * size; the read is capped at MAX_PHDRS entries so a hostile count
     * cannot drive a giant allocation (f15/f17). */
    size_t nph = eh.e_phnum < MAX_PHDRS ? eh.e_phnum : MAX_PHDRS;
    size_t table_len = (size_t)nph * eh.e_phentsize;
    uint8_t *phdr_buf = kmalloc(table_len ? table_len : 1);
    if (!phdr_buf)
        return -ENOMEM;
    size_t phdr_got = 0;
    r = table_len ? elf_rust_read_up_to(&(elf_rust_src_t){
            .fd = src->fd, .buf = src->buf ? (const char *)src->buf + eh.e_phoff : NULL,
            .buf_len = src->buf && eh.e_phoff < src->buf_len
                       ? src->buf_len - (size_t)eh.e_phoff : 0 },
            phdr_buf, table_len, eh.e_phoff, &phdr_got) : 0;
    if (r < 0 || phdr_got < table_len) {
        kfree(phdr_buf);
        return r < 0 ? r : -ENOEXEC;
    }

    a20_elf_image_plan_t *plan = kmalloc(sizeof(*plan));
    if (!plan) {
        kfree(phdr_buf);
        return -ENOMEM;
    }
    r = a20_elf_plan_image(ehdr_buf, ehdr_got, phdr_buf, phdr_got,
                           ARCH_ELF_CLASS, ARCH_ELF_MACHINE,
                           mm_wx_policy(), elf_rust_xlator_host(), plan);
    kfree(phdr_buf);
    if (r < 0) {
        kfree(plan);
        return r;
    }

    r = elf_native_abi_available(plan->is_native_abi, path);
    if (r < 0) {
        kfree(plan);
        return r;
    }

    elf_load_transaction_t tx = {
        .src = src, .path = path, .info = info, .plan = plan,
        .resources = { .interp_fd = -1, .tls_align = 1 },
    };
    a20_elf_load_transaction_ops_t ops = {
        .context = &tx,
        .resources = &tx.resources,
        .begin = elf_load_tx_begin,
        .map_segments = elf_load_tx_map,
        .stage_tls = elf_load_tx_stage_tls,
        .relocate = elf_load_tx_relocate,
        .finish_layout = elf_load_tx_finish_layout,
        .read_interpreter = elf_load_tx_read_interpreter,
        .open_interpreter = elf_load_tx_open_interpreter,
        .build_interpreter_path = elf_load_tx_build_interp_path,
        .arch_interpreter_path = elf_load_tx_arch_interp_path,
        .path_capacity = MAX_PATH_LEN,
#ifdef CONFIG_NOMMU
        .nommu = 1,
#endif
        .load_interpreter = elf_load_tx_interpreter,
        .close_interpreter = elf_load_tx_close_interpreter,
        .map_stack = elf_load_tx_stack,
        .install_tls = elf_load_tx_install_tls,
        .brk_offset = elf_load_tx_brk_offset,
        .commit = elf_load_tx_commit,
        .free_tls = elf_load_tx_free_tls,
        .discard_vmas = elf_load_tx_discard_vmas,
        .discard_nommu = elf_load_tx_discard_nommu,
        .destroy_pgdir = elf_load_tx_destroy_pgdir,
    };
    uint64_t load_bias = 0;
    r = a20_elf_load_transaction(plan, &ops, &load_bias);
    kfree(plan);
    return r;
}
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

int elf_check_header(const Elf64_Ehdr *eh) {
    return a20_elf_validate_header((const uint8_t *)eh, sizeof(*eh),
                                   ARCH_ELF_CLASS, ARCH_ELF_MACHINE);
}

/* The one definition of "this image speaks the A20 native ABI".
 *
 * A single PT_A20_START_INFO in the program header table is the marker, and
 * every reader of that table has to reach the same verdict: the native entry
 * stub pulls an a20_start_info_t out of that segment and expects nine
 * a20_handle_t values there, which is not what a Linux ABI entry does.  So
 * the scan lives here once instead of being open-coded in each loader, where
 * a later reader could disagree with the loader about the very same file and
 * neither would be looking for the other.
 *
 * @base + @off locates @n headers of @entsize bytes; @avail is how many bytes
 * are readable from there.  Headers that would reach past @avail are not
 * read.  A table that cannot be read in full reports Linux ABI: the header
 * already claimed a program header table the file does not have, which is
 * the elf_check_header() rejection the caller reaches anyway, and returning
 * that verdict from here would duplicate it somewhere that does not own it. */

/* Never enter a Native-marked image with the Linux stack convention. */
static int elf_native_abi_available(int is_native, const char *path)
{
#if defined(CONFIG_ABI_NATIVE) || defined(CONFIG_ABI_BOTH)
    (void)is_native;
    (void)path;
    return 0;
#else
    if (!is_native)
        return 0;
    kinfo("[ELF] Native ABI image rejected by ABI=linux kernel: %s\n",
          path ? path : "(unknown)");
    return -ENOEXEC;
#endif
}

/* Decide whether a file that elf_load() already rejected as -ENOEXEC is
 * structurally a foreign-architecture executable, reporting the pair the
 * translation channel is keyed on: its e_machine and its ABI.
 *
 * The checks mirror elf_check_header() *except* for the machine test, which
 * only has to exclude the native machine.  Deciding whether a given pair is
 * *translatable* is not this function's business and is not a compiled-in
 * list: it belongs to the translator channel, whose answer depends on what an
 * administrator configured (kernel/proc/xlator.c).  Keeping that out of here
 * is what lets a new guest architecture -- or a new ABI for a guest
 * architecture already present -- be added without touching the kernel.
 *
 * The fail-closed property is unchanged by that move, and rests on the same
 * two facts as before: everything this predicate rejects -- bad magic, wrong
 * class/endianness, wrong e_type, a short program header table -- is still
 * rejected here, and a pair nobody configured a translator for has no entry
 * to find in xlator_lookup().  A corrupt header therefore cannot be mistaken
 * for "please translate this": a garbage e_machine matches nothing.
 *
 * The ABI is read from the program headers rather than configured, because it
 * is a fact about the file in the same way e_machine is: an image carrying
 * PT_A20_START_INFO is a native-ABI program no matter what the command line
 * says.  Deriving it here is what lets a native-ABI translator be a new row of
 * the registry rather than a new branch in the loader. */
int elf_probe_foreign(int fd, elf_guest_key_t *out)
{
    /* The structural verdict (magic/class/endianness/type/phdr-table gate,
     * native-machine exclusion, PT_A20_START_INFO ABI scan) is Rust-owned;
     * this body only supplies the bytes, exactly like the C reference. */
    if (vfs_lseek(fd, 0, SEEK_SET) < 0)
        return -ENOEXEC;

    uint8_t ehbuf[sizeof(Elf64_Ehdr)];
    int n = vfs_read(fd, (char *)ehbuf, sizeof(ehbuf));
    if (n != (int)sizeof(ehbuf))
        return -ENOEXEC;

    a20_elf_ehdr_t eh;
    if (a20_elf_parse_header(ehbuf, sizeof(ehbuf), &eh) < 0)
        return -ENOEXEC;

    size_t nph = eh.e_phnum < MAX_PHDRS ? eh.e_phnum : MAX_PHDRS;
    size_t want = (size_t)nph * eh.e_phentsize;
    void *tab = want ? kmalloc(want) : NULL;
    int got = 0;
    if (tab && vfs_lseek(fd, (long)eh.e_phoff, SEEK_SET) >= 0)
        got = vfs_read(fd, (char *)tab, want);
    if (!tab)
        got = 0;

    a20_elf_foreign_key_t key;
    int r = a20_elf_probe_foreign(ehbuf, sizeof(ehbuf), tab,
                                  got > 0 ? (size_t)got : 0,
                                  ARCH_ELF_CLASS, ARCH_ELF_MACHINE, &key);
    kfree(tab);
    if (r < 0)
        return r;

    if (out) {
        out->machine = key.machine;
        out->abi = key.abi ? ELF_ABI_NATIVE : ELF_ABI_LINUX;
    }
    return 0;
}


int elf_load_from_buf(const void *buf, size_t len, elf_load_info_t *info) {
    elf_rust_src_t src = { .fd = -1, .buf = buf, .buf_len = len };
    int r = elf_rust_load_planned(&src, "<buffer>", info);
    /* No filesystem path exists for AT_EXECFN; publish none rather than a
     * synthetic "<buffer>" that no loader could resolve. */
    if (r == 0)
        info->path = NULL;
    return r;
}



int elf_load(int fd, const char *path, elf_load_info_t *info) {
    /* Driver-registry gate stays with the C reference (out of this module's
     * scope); every other decision is inside elf_rust_load_planned(). */
    unsigned char buf[sizeof(Elf64_Ehdr)];
    if (vfs_lseek(fd, 0, SEEK_SET) < 0) return -EIO;
    int r = vfs_read(fd, (char *)buf, sizeof(buf));
    if (r < (int)sizeof(Elf32_Ehdr))
        return -ENOEXEC;
    if (*(uint32_t *)buf != ELF_MAGIC)
        return -ENOEXEC;
    if (buf[4] != ELFCLASS32 && buf[4] != ELFCLASS64)
        return -ENOEXEC;
    if (buf[4] == ELFCLASS64 && elf_path_has_driver_suffix(path)) {
        r = elf_validate_user_driver(fd, (const Elf64_Ehdr *)buf);
        if (r < 0)
            return r;
    }
    elf_rust_src_t src = { .fd = fd, .buf = NULL, .buf_len = 0 };
    return elf_rust_load_planned(&src, path, info);

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
#define AT_SYSINFO_EHDR 33

/*
 * Linux-ABI initial stack, Rust layout/writer + C page-copy mechanisms.
 *
 * elf_core owns placement, vector order, auxv, and final SP. These callbacks
 * keep only mapped-page copies and random-byte acquisition in C.
 */
vaddr_t elf_setup_stack(vaddr_t stack_top, int argc, char *const argv[],
                        char *const envp[], const elf_load_info_t *info,
                        vaddr_t vdso_ehdr,
                        uintptr_t out_auxv[A20_AUXV_MAX_PAIRS][2],
                        uint32_t *out_auxv_n) {
    if (argc < 0 || argc > MAX_ARG_STRINGS)
        return 0;

    pt_root_t *pgdir = info->pgdir;
    vaddr_t stack_bottom = stack_top -
        (uint64_t)USER_STACK_INITIAL_PAGES * PAGE_SIZE;

    const uint8_t *argv_ptrs[MAX_ARG_STRINGS];
    size_t argv_lens[MAX_ARG_STRINGS];
    const uint8_t *envp_ptrs[MAX_ARG_STRINGS];
    size_t envp_lens[MAX_ARG_STRINGS];
    int envc = 0;
    if (envp) {
        while (envc < MAX_ARG_STRINGS && envp[envc]) {
            envp_ptrs[envc] = (const uint8_t *)envp[envc];
            envp_lens[envc] = strlen(envp[envc]);
            envc++;
        }
    }
    for (int i = 0; i < argc; i++) {
        argv_ptrs[i] = (const uint8_t *)argv[i];
        argv_lens[i] = argv[i] ? strlen(argv[i]) : 0;
    }

    const char *platform = ARCH_NAME;
    a20_elf_auxv_input_t aux = {
        .phdr_va     = (uint64_t)info->phdr_va,
        .phentsize   = info->phentsize,
        .phnum       = info->phnum,
        .page_size   = PAGE_SIZE,
        .interp_base = (uint64_t)info->interp_base,
        .exec_entry  = (uint64_t)info->exec_entry,
        .hwcap       = (uint64_t)ARCH_ELF_HWCAP(),
        .hwcap2      = 0,
        .vdso_ehdr   = (uint64_t)vdso_ehdr,
        .clktck      = 100,
        .secure      = 0,
    };

    elf_stack_writer_t writer = {
        .pgdir = pgdir,
        .stack_bottom = stack_bottom,
    };
    uint64_t sp = 0;
    int32_t r = a20_elf_write_linux_stack(
        stack_top, sizeof(uintptr_t), argv_ptrs, argv_lens, (size_t)argc,
        envp_ptrs, envp_lens, (size_t)envc,
        (const uint8_t *)platform, strlen(platform),
        (const uint8_t *)info->path, info->path ? strlen(info->path) : 0,
        &aux, &writer, elf_stack_copy_ffi, elf_stack_random_ffi,
        out_auxv ? (uintptr_t *)out_auxv : NULL, out_auxv_n, &sp);
    return r < 0 ? 0 : (vaddr_t)sp;
}

#if defined(CONFIG_ABI_NATIVE) || defined(CONFIG_ABI_BOTH)
static vaddr_t elf_native_copy_bootstrap(const elf_load_info_t *info,
                                         vaddr_t stack_top, const void *data,
                                         size_t size)
{
    if (!size) return stack_top;
    if (!data || size > 65536 || stack_top < size + PAGE_SIZE) return 0;
    vaddr_t address = (stack_top - size) & ~(vaddr_t)15;
    vaddr_t bottom = stack_top - (uint64_t)USER_STACK_INITIAL_PAGES * PAGE_SIZE;
    if (stack_copy(info->pgdir, address, data, size, &bottom) < 0) return 0;
    return address;
}

/*
 * Native-ABI initial stack. Rust owns the layout and writes strings, pointer
 * arrays, and descriptor fields; C supplies the mapped-page copy mechanism.
 * elf_setup_stack_a20_dynamic() below composes the Linux and native layouts.
 */
vaddr_t elf_setup_stack_a20(vaddr_t stack_top, int argc, char *const argv[],
                            char *const envp[], const elf_load_info_t *info,
                            uint32_t stdin_h, uint32_t stdout_h,
                            uint32_t stderr_h, uint32_t self_task_h,
                            uint32_t root_h, uint32_t cwd_h,
                            uint32_t inherited_fd_limit,
                            uint32_t registry_h,
                            const void *bootstrap_data, size_t bootstrap_size)
{
    if (argc < 0 || argc > MAX_ARG_STRINGS)
        return 0;

    stack_top = elf_native_copy_bootstrap(info, stack_top, bootstrap_data, bootstrap_size);
    if (!stack_top) return 0;

    pt_root_t *pgdir = info->pgdir;
    vaddr_t stack_bottom = stack_top -
        (uint64_t)USER_STACK_INITIAL_PAGES * PAGE_SIZE;

    const uint8_t *argv_ptrs[MAX_ARG_STRINGS];
    size_t argv_lens[MAX_ARG_STRINGS];
    const uint8_t *envp_ptrs[MAX_ARG_STRINGS];
    size_t envp_lens[MAX_ARG_STRINGS];
    int envc = 0;
    if (envp) {
        while (envc < MAX_ARG_STRINGS && envp[envc]) {
            envp_ptrs[envc] = (const uint8_t *)envp[envc];
            envp_lens[envc] = strlen(envp[envc]);
            envc++;
        }
    }
    for (int i = 0; i < argc; i++) {
        argv_ptrs[i] = (const uint8_t *)argv[i];
        argv_lens[i] = argv[i] ? strlen(argv[i]) : 0;
    }

    a20_start_info_t si;
    memset(&si, 0, sizeof(si));
    si.size = sizeof(si);
    si.version = A20_START_INFO_VERSION;
    si.reserved0 = inherited_fd_limit;
    si.root_dir = root_h;
    si.cwd_dir = cwd_h;
    si.stdin_handle = stdin_h;
    si.stdout_handle = stdout_h;
    si.stderr_handle = stderr_h;
    si.self_task = self_task_h;
    si.page_size = PAGE_SIZE;
    si.service_registry = registry_h;
    si.bootstrap_data = bootstrap_size ? stack_top : 0;
    si.bootstrap_size = bootstrap_size;

    elf_stack_writer_t writer = {
        .pgdir = pgdir,
        .stack_bottom = stack_bottom,
    };
    uint64_t sp = 0;
    int32_t r = a20_elf_write_native_stack(
        stack_top, sizeof(uintptr_t), argv_ptrs, argv_lens, (size_t)argc,
        envp_ptrs, envp_lens, (size_t)envc,
        (const uint8_t *)&si, sizeof(si), &writer, elf_stack_copy_ffi, &sp);
    return r < 0 ? 0 : (vaddr_t)sp;
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
                                     uint32_t inherited_fd_limit,
                                     vaddr_t *start_info_out,
                                     const void *bootstrap_data, size_t bootstrap_size)
{
    if (!start_info_out)
        return 0;

    stack_top = elf_native_copy_bootstrap(info, stack_top, bootstrap_data, bootstrap_size);
    if (!stack_top) return 0;

    vaddr_t si_va = stack_top - sizeof(a20_start_info_t);
    /* Native-dynamic tasks keep the syscall path for now: no vDSO is
     * mapped for them, so AT_SYSINFO_EHDR must be 0 (musl falls back).
     * A20's native ABI boots from a20_start_info_t rather than from a Linux
     * auxv, so there is no vector to publish and /proc/<pid>/auxv stays empty
     * for these tasks -- matching a program that was never handed one. */
    vaddr_t sp_va = elf_setup_stack(si_va, argc, argv, envp, info, 0,
                                    NULL, NULL);
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
    si.version = A20_START_INFO_VERSION;
    si.argc = (uint32_t)argc;
    si.envc = (uint32_t)envc;
    si.reserved0 = inherited_fd_limit;
    si.argv = sp_va + sizeof(vaddr_t);
    si.envp = si.argv + (uint64_t)(argc + 1) * sizeof(vaddr_t);
    si.stdin_handle = stdin_h;
    si.stdout_handle = stdout_h;
    si.stderr_handle = stderr_h;
    si.self_task = self_task_h;
    si.root_dir = root_h;
    si.cwd_dir = cwd_h;
    si.page_size = PAGE_SIZE;
    si.bootstrap_data = bootstrap_size ? stack_top : 0;
    si.bootstrap_size = bootstrap_size;

    vaddr_t stack_bottom = stack_top -
        (uint64_t)USER_STACK_INITIAL_PAGES * PAGE_SIZE;
    if (stack_copy(info->pgdir, si_va, &si, sizeof(si), &stack_bottom) < 0)
        return 0;

    *start_info_out = si_va;
    return sp_va;
}
#endif

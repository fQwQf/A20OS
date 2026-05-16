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
#ifdef CONFIG_ABI_NATIVE
#include "abi/native/startup.h"
#endif

/* musl struct pthread is ~300-400 bytes; 512 gives headroom */
#define TLS_TCB_SIZE       512

#define PTE_STACK          (PTE_V | PTE_R | PTE_W | PTE_X | PTE_U | PTE_A | PTE_D | PTE_MAT1 | PTE_LEAF)

#define ASLR_BITS          16
#define ASLR_MASK          (((1UL << ASLR_BITS) - 1) << 16)

/* Max program headers we read in one shot (3584 bytes on stack) */
#define MAX_PHDRS          64

/* ------------------------------------------------------------------ */
/*  Utilities                                                         */
/* ------------------------------------------------------------------ */

static uint64_t elf_aslr_bias(void) {
    uint64_t r = random_u64();
    uint64_t bits = (r ^ (r >> 29) ^ (r >> 47)) & ((1UL << ASLR_BITS) - 1);
    return bits << 16;
}

static uint64_t seg_flags(uint32_t p_flags) {
    uint64_t f = PTE_V | PTE_U | PTE_MAT1 | PTE_A | PTE_LEAF;
    if (p_flags & PF_R) f |= PTE_R;
    if (p_flags & PF_W) f |= (PTE_W | PTE_D);
    if (p_flags & PF_X) f |= PTE_X;
    if (f & PTE_W) f |= PTE_R;
    return f;
}

static uint64_t pte_to_vm_flags(uint64_t pte_flags) {
    uint64_t vm = VM_ANON;
    if (pte_flags & PTE_R) vm |= VM_READ;
    if (pte_flags & PTE_W) vm |= VM_WRITE;
    if (pte_flags & PTE_X) vm |= VM_EXEC;
    return vm;
}

static int elf_add_vma(mm_struct_t *mm, uint64_t start, uint64_t end,
                       uint64_t vm_flags, uint64_t pte_flags) {
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

static void *phys_for_va(uint64_t *pgdir, uint64_t va) {
    paddr_t pa = pt_translate(pgdir, va);
    if (pa == 0) return NULL;
    return (void *)((uint64_t)pa + PAGE_OFFSET);
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

/*
 * Map an ELF segment into the page table.
 *
 * Handles both buffer-backed and fd-backed segments through @src.
 * - va:      target virtual address (may be page-unaligned)
 * - memsz:   total memory size of segment (BSS included)
 * - filesz:  size of initialized data in file
 * - flags:   PTE flags for mapping
 */
static int map_segment(mm_struct_t *mm, uint64_t *pgdir,
                       uint64_t va, uint64_t memsz,
                       const seg_src_t *src, uint64_t filesz,
                       uint64_t flags) {
    uint64_t start = va & ~(PAGE_SIZE - 1);
    uint64_t end   = ROUND_UP(va + memsz, PAGE_SIZE);
    char tmp[PAGE_SIZE];

    for (uint64_t page = start; page < end; page += PAGE_SIZE) {
        void *frame = frame_alloc();
        if (!frame) return -ENOMEM;

        /* Preserve existing page contents if page already mapped */
        uint64_t *old_pte = pt_walk(pgdir, page, 0);
        if (old_pte && (*old_pte & PTE_V)) {
            paddr_t old_pa = arch_pte_addr(*old_pte);
            memcpy(frame, (void *)(old_pa + PAGE_OFFSET), PAGE_SIZE);
        }

        if (page < va + filesz) {
            uint64_t copy_off = (page < va) ? (va - page) : 0;
            uint64_t src_off  = (page < va) ? 0 : (page - va);
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
                    memcpy((char *)frame + copy_off, tmp, (size_t)nr);
                }
            }
        }

        int r = pt_map(pgdir, page, va_to_pa(frame), flags);
        if (r < 0) { frame_free(frame); return r; }
    }
    arch_tlb_flush();

    return elf_add_vma(mm, start, end, pte_to_vm_flags(flags), flags);
}

/* ------------------------------------------------------------------ */
/*  Stack mapping                                                     */
/* ------------------------------------------------------------------ */

static int map_stack(mm_struct_t *mm, uint64_t *pgdir, uint64_t *stack_top_out) {
    uint64_t stack_top    = USER_STACK_TOP + PAGE_SIZE;
    uint64_t stack_bottom = stack_top -
        (uint64_t)USER_STACK_INITIAL_PAGES * PAGE_SIZE;

    for (int i = 0; i < USER_STACK_INITIAL_PAGES; i++) {
        uint64_t va = USER_STACK_TOP -
                      (uint64_t)(USER_STACK_INITIAL_PAGES - 1 - i) * PAGE_SIZE;
        void *frame = frame_alloc();
        if (!frame) return -ENOMEM;
        int r = pt_map(pgdir, va, va_to_pa(frame), PTE_STACK);
        if (r < 0) { frame_free(frame); return r; }
    }
    *stack_top_out = stack_top;
    arch_tlb_flush();
    return elf_add_vma(mm, stack_bottom, stack_top,
                       VM_ANON | VM_READ | VM_WRITE | VM_STACK, PTE_STACK);
}

/* ------------------------------------------------------------------ */
/*  TLS setup                                                         */
/* ------------------------------------------------------------------ */

static int setup_tls(mm_struct_t *mm, uint64_t *pgdir,
                     const void *tls_data, uint64_t tls_filesz,
                     uint64_t tls_memsz, uint64_t tls_align,
                     uint64_t *tls_va_out, uint64_t *tls_tp_out) {
    uint64_t tcb_offset = ROUND_UP(tls_memsz, tls_align);
    uint64_t total_size = tcb_offset + TLS_TCB_SIZE;
    uint64_t total_pages = ROUND_UP(total_size, PAGE_SIZE) / PAGE_SIZE;
    uint64_t pte_flags = PTE_V | PTE_R | PTE_W | PTE_U | PTE_A | PTE_D |
                         PTE_MAT1 | PTE_LEAF;

    for (uint64_t page = 0; page < total_pages; page++) {
        void *frame = frame_alloc();
        if (!frame) return -ENOMEM;
        int r = pt_map(pgdir, USER_TLS_BASE + page * PAGE_SIZE,
                       va_to_pa(frame), pte_flags);
        if (r < 0) { frame_free(frame); return r; }
    }

    uint64_t tls_va = USER_TLS_BASE;
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

    /* Initialize TCB: self-pointer at offset 0, dtv pointer at offset 8 */
    uint64_t tcb_va = tls_va + tcb_offset;
    void *tcb_dst = phys_for_va(pgdir, tcb_va);
    if (!tcb_dst) return -EFAULT;
    memset(tcb_dst, 0, TLS_TCB_SIZE);
    *(uint64_t *)tcb_dst       = tcb_va;
    *((uint64_t *)tcb_dst + 1) = tcb_va;

    *tls_va_out = tls_va;
    *tls_tp_out = tcb_va;
    arch_tlb_flush();
    return elf_add_vma(mm, USER_TLS_BASE,
                       USER_TLS_BASE + total_pages * PAGE_SIZE,
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
 *   2. Sibling of executable (strip last path component, append interp)
 *   3. Sibling libc.so (musl convention: libc.so IS the dynamic linker)
 *   4. Arch-specific fallbacks (glibc paths, mount-relative paths)
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

    /* 2. Sibling of executable */
    if (exec_path && path_build_sibling(exec_path, interp_path,
                                        resolved, resolved_size) == 0) {
        fd = vfs_open(resolved, O_RDONLY, 0);
        if (fd >= 0) return fd;
    }

    /* 3. Sibling libc.so (musl: libc.so doubles as ldso) */
    if (exec_path && path_build_sibling(exec_path, "/lib/libc.so",
                                        resolved, resolved_size) == 0) {
        fd = vfs_open(resolved, O_RDONLY, 0);
        if (fd >= 0) return fd;
    }

    /* 4. Arch-specific fallbacks */
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
static int elf_load_interp_from_fd(mm_struct_t *mm, uint64_t *pgdir,
                                   int fd,
                                   uint64_t *entry_out, uint64_t *base_out) {
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

    uint64_t load_bias = INTERP_BASE_ADDR + elf_aslr_bias();
    uint64_t base = 0, max_va = 0;

    for (int i = 0; i < nph; i++) {
        if (phdrs[i].p_type != PT_LOAD) continue;

        uint64_t seg_va = phdrs[i].p_vaddr + load_bias;
        seg_src_t src = seg_from_fd(fd, (long)phdrs[i].p_offset);
        r = map_segment(mm, pgdir, seg_va, phdrs[i].p_memsz,
                        &src, phdrs[i].p_filesz,
                        seg_flags(phdrs[i].p_flags));
        if (r < 0) return r;

        uint64_t seg_start = seg_va & ~(PAGE_SIZE - 1);
        uint64_t seg_end   = ROUND_UP(seg_va + phdrs[i].p_memsz, PAGE_SIZE);
        if (base == 0) base = seg_start;
        if (seg_end > max_va) max_va = seg_end;
    }

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
    if (eh->e_phentsize < sizeof(Elf64_Phdr))   return -ENOEXEC;
    if (eh->e_phnum == 0 || eh->e_phnum > MAX_PHDRS) return -ENOEXEC;
    return 0;
}

int elf_load_from_buf(const void *buf, size_t len, elf_load_info_t *info) {
    if (len < sizeof(Elf64_Ehdr)) return -ENOEXEC;
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)buf;

    int r = elf_check_header(eh);
    if (r < 0) return r;

    uint64_t *pgdir = pt_create();
    if (!pgdir) return -ENOMEM;
    pt_map_kernel(pgdir);

    mm_struct_t mm = { .pgdir = pgdir, .mmap_base = MMAP_BASE_ADDR };

    uint64_t load_bias = (eh->e_type == ET_DYN) ? (USER_DYN_BASE + elf_aslr_bias()) : 0;
    uint64_t base = 0, max_va = 0, brk_va = 0;
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

        uint64_t seg_va = ph->p_vaddr + load_bias;
        seg_src_t src = seg_from_buf((const char *)buf + ph->p_offset);
        r = map_segment(&mm, pgdir, seg_va, ph->p_memsz,
                        &src, ph->p_filesz, seg_flags(ph->p_flags));
        if (r < 0) { pt_destroy_user(pgdir); return r; }

        uint64_t seg_start = seg_va & ~(PAGE_SIZE - 1);
        uint64_t seg_end   = ROUND_UP(seg_va + ph->p_memsz, PAGE_SIZE);
        if (base == 0) base = seg_start;
        if (seg_end > max_va) max_va = seg_end;
        if ((ph->p_flags & PF_W) && seg_end > brk_va) brk_va = seg_end;
    }

    uint64_t stack_top;
    r = map_stack(&mm, pgdir, &stack_top);
    if (r < 0) { pt_destroy_user(pgdir); return r; }

    uint64_t tls_va = 0, tls_tp = 0;
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
    return 0;
}

int elf_load(int fd, const char *path, elf_load_info_t *info) {
    Elf64_Ehdr eh;
    if (vfs_lseek(fd, 0, SEEK_SET) < 0) return -EIO;
    int r = vfs_read(fd, (char *)&eh, sizeof(eh));
    if (r < (int)sizeof(eh)) return -ENOEXEC;

    r = elf_check_header(&eh);
    if (r < 0) {
        kinfo("[ELF] header check failed: r=%d magic=0x%x class=%d data=%d "
              "type=%d machine=%d\n",
              r, *(uint32_t *)eh.e_ident, eh.e_ident[4], eh.e_ident[5],
              eh.e_type, eh.e_machine);
        return r;
    }

    Elf64_Phdr phdrs[MAX_PHDRS];
    int nph = eh.e_phnum < MAX_PHDRS ? eh.e_phnum : MAX_PHDRS;
    vfs_lseek(fd, (long)eh.e_phoff, SEEK_SET);
    r = vfs_read(fd, (char *)phdrs, nph * sizeof(Elf64_Phdr));
    if (r < nph * (int)sizeof(Elf64_Phdr)) return -ENOEXEC;

    uint64_t *pgdir = pt_create();
    if (!pgdir) return -ENOMEM;
    pt_map_kernel(pgdir);

    mm_struct_t mm = { .pgdir = pgdir, .mmap_base = MMAP_BASE_ADDR };

    uint64_t load_bias = (eh.e_type == ET_DYN) ? (USER_DYN_BASE + elf_aslr_bias()) : 0;
    uint64_t base = 0, max_va = 0, brk_va = 0, head_va = 0;
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
                if (!tls_data) goto fail;
                vfs_lseek(fd, (long)phdrs[i].p_offset, SEEK_SET);
                int nr = vfs_read(fd, (char *)tls_data, (size_t)tls_filesz);
                if (nr < 0) { kfree(tls_data); tls_data = NULL; goto fail; }
            }
            continue;
        }
        if (phdrs[i].p_type != PT_LOAD) continue;

        uint64_t seg_va = phdrs[i].p_vaddr + load_bias;
        seg_src_t src = seg_from_fd(fd, (long)phdrs[i].p_offset);
        r = map_segment(&mm, pgdir, seg_va, phdrs[i].p_memsz,
                        &src, phdrs[i].p_filesz,
                        seg_flags(phdrs[i].p_flags));
        if (r < 0) goto fail;

        uint64_t seg_start = seg_va & ~(PAGE_SIZE - 1);
        uint64_t seg_end   = ROUND_UP(seg_va + phdrs[i].p_memsz, PAGE_SIZE);
        if (phdrs[i].p_offset == 0) head_va = seg_va;
        if (base == 0) base = seg_start;
        if (seg_end > max_va) max_va = seg_end;
        if ((phdrs[i].p_flags & PF_W) && seg_end > brk_va) brk_va = seg_end;
    }

    uint64_t interp_entry = 0, interp_base = 0;
    if (has_interp) {
        char resolved[MAX_PATH_LEN];
        int interp_fd = resolve_interp(path, interp_path,
                                       resolved, sizeof(resolved));
        kinfo("[ELF] interp: exec='%s' pt_interp='%s' resolved='%s' fd=%d\n",
              path ? path : "(null)", interp_path, resolved, interp_fd);
        if (interp_fd < 0) {
            kinfo("[ELF] INTERP NOT FOUND for '%s' wanted '%s'\n",
                  path ? path : "(null)", interp_path);
            r = interp_fd;
            goto fail;
        }
        r = elf_load_interp_from_fd(&mm, pgdir, interp_fd,
                                    &interp_entry, &interp_base);
        vfs_close(interp_fd);
        if (r < 0) goto fail;
    }

    uint64_t stack_top;
    r = map_stack(&mm, pgdir, &stack_top);
    if (r < 0) goto fail;

    uint64_t tls_va = 0, tls_tp = 0;
    r = setup_tls(&mm, pgdir, tls_data, tls_filesz, tls_memsz, tls_align,
                  &tls_va, &tls_tp);
    if (r < 0) { kfree(tls_data); pt_destroy_user(pgdir); return r; }

    *info = (elf_load_info_t){
        .entry       = has_interp ? interp_entry : (eh.e_entry + load_bias),
        .exec_entry  = eh.e_entry + load_bias,
        .base        = base,
        .end_va      = max_va,
        .brk         = ROUND_UP(brk_va ? brk_va : max_va, PAGE_SIZE),
        .phdr_va     = head_va ? (head_va + eh.e_phoff) : (base + eh.e_phoff),
        .phnum       = (uint32_t)nph,
        .phentsize   = eh.e_phentsize,
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

    kfree(tls_data);
    return 0;

fail:
    kfree(tls_data);
    {
        vm_area_t *vma = mm.mmap;
        while (vma) {
            vm_area_t *next = vma->next;
            kfree(vma);
            vma = next;
        }
    }
    pt_destroy_user(pgdir);
    return r;
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

uint64_t elf_setup_stack(uint64_t stack_top, int argc, char *const argv[],
                          char *const envp[], const elf_load_info_t *info) {
    uint64_t *pgdir = info->pgdir;
    uint64_t sp_va  = stack_top;

    int envc = 0;
    uint64_t env_ptrs[64];
    if (envp) {
        while (envp[envc] && envc < 63) {
            int len = (int)strlen(envp[envc]) + 1;
            sp_va -= len;
            void *dst = phys_for_va(pgdir, sp_va);
            if (!dst) return 0;
            memcpy(dst, envp[envc], len);
            env_ptrs[envc] = sp_va;
            envc++;
        }
    }
    env_ptrs[envc] = 0;

    uint64_t arg_ptrs[64];
    for (int i = argc - 1; i >= 0; i--) {
        int len = (int)strlen(argv[i]) + 1;
        sp_va -= len;
        void *dst = phys_for_va(pgdir, sp_va);
        if (!dst) return 0;
        memcpy(dst, argv[i], len);
        arg_ptrs[i] = sp_va;
    }
    arg_ptrs[argc] = 0;

    sp_va &= ~15UL;

    const char *platform = ARCH_NAME;
    int plat_len = (int)strlen(platform) + 1;

    /* Align total fixed-size data to 16 bytes */
    {
        size_t fixed = (size_t)plat_len + (size_t)(envc + argc + 3) * 8;
        sp_va -= (16 - (fixed & 15)) & 15;
    }

    sp_va -= plat_len;
    uint64_t platform_va = sp_va;
    {
        void *dst = phys_for_va(pgdir, sp_va);
        if (!dst) return 0;
        memcpy(dst, platform, plat_len);
    }

    sp_va -= 16;
    uint64_t random_va = sp_va;
    {
        void *dst = phys_for_va(pgdir, sp_va);
        if (!dst) return 0;
        random_fill(dst, 16);
    }

    uint64_t auxv[][2] = {
        { AT_PHDR,   info->phdr_va  },
        { AT_PHENT,  info->phentsize },
        { AT_PHNUM,  info->phnum     },
        { AT_PAGESZ, PAGE_SIZE       },
        { AT_BASE,   info->interp_base },
        { AT_FLAGS,  0               },
        { AT_ENTRY,  info->exec_entry },
        { AT_UID,    0               },
        { AT_EUID,   0               },
        { AT_GID,    0               },
        { AT_EGID,   0               },
        { AT_PLATFORM, platform_va   },
        { AT_HWCAP,  0               },
        { AT_CLKTCK, 100             },
        { AT_SECURE, 0               },
        { AT_RANDOM, random_va       },
        { AT_HWCAP2, 0               },
        { AT_NULL,   0               },
    };
    int naux = (int)(sizeof(auxv) / sizeof(auxv[0]));

    sp_va -= naux * 16;
    {
        void *dst = phys_for_va(pgdir, sp_va);
        if (!dst) return 0;
        memcpy(dst, auxv, naux * 16);
    }

    sp_va -= (envc + 1) * 8;
    {
        void *dst = phys_for_va(pgdir, sp_va);
        if (!dst) return 0;
        memcpy(dst, env_ptrs, (envc + 1) * 8);
    }

    sp_va -= (argc + 1) * 8;
    {
        void *dst = phys_for_va(pgdir, sp_va);
        if (!dst) return 0;
        memcpy(dst, arg_ptrs, (argc + 1) * 8);
    }

    sp_va -= 8;
    {
        void *dst = phys_for_va(pgdir, sp_va);
        if (!dst) return 0;
        *(uint64_t *)dst = (uint64_t)argc;
    }

    return sp_va;
}

#ifdef CONFIG_ABI_NATIVE
uint64_t elf_setup_stack_a20(uint64_t stack_top, int argc, char *const argv[],
                              char *const envp[], const elf_load_info_t *info,
                              uint32_t stdin_h, uint32_t stdout_h,
                              uint32_t stderr_h, uint32_t self_task_h)
{
    uint64_t *pgdir = info->pgdir;
    uint64_t sp_va = stack_top;

    int envc = 0;
    uint64_t env_ptrs[64];
    if (envp) {
        while (envp[envc] && envc < 63) {
            int len = (int)strlen(envp[envc]) + 1;
            sp_va -= len;
            void *dst = phys_for_va(pgdir, sp_va);
            if (!dst) return 0;
            memcpy(dst, envp[envc], len);
            env_ptrs[envc] = sp_va;
            envc++;
        }
    }
    env_ptrs[envc] = 0;

    uint64_t arg_ptrs[64];
    for (int i = argc - 1; i >= 0; i--) {
        int len = (int)strlen(argv[i]) + 1;
        sp_va -= len;
        void *dst = phys_for_va(pgdir, sp_va);
        if (!dst) return 0;
        memcpy(dst, argv[i], len);
        arg_ptrs[i] = sp_va;
    }
    arg_ptrs[argc] = 0;

    sp_va &= ~15UL;

    sp_va -= (envc + 1) * 8;
    {
        void *dst = phys_for_va(pgdir, sp_va);
        if (!dst) return 0;
        memcpy(dst, env_ptrs, (envc + 1) * 8);
    }
    uint64_t envp_va = sp_va;

    sp_va -= (argc + 1) * 8;
    {
        void *dst = phys_for_va(pgdir, sp_va);
        if (!dst) return 0;
        memcpy(dst, arg_ptrs, (argc + 1) * 8);
    }
    uint64_t argv_va = sp_va;

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
        si.page_size = PAGE_SIZE;
        void *dst = phys_for_va(pgdir, sp_va);
        if (!dst) return 0;
        memcpy(dst, &si, sizeof(si));
    }

    return sp_va;
}
#endif

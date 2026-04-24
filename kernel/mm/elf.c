/*
 * A20OS ELF64 Loader with per-process SV39 page tables.
 *
 * Each loaded binary gets its own page table with:
 *   - User segments mapped at their ELF virtual addresses (PTE_USER)
 *   - Kernel space identity-mapped (PTE_KERN, no PTE_U)
 *   - MMIO devices identity-mapped (PTE_KERN, no PTE_U)
 *   - User stack mapped at VA 0x3FFFF000 downward
 */

#include "elf.h"
#include "mm.h"
#include "vm.h"
#include "vfs.h"
#include "string.h"
#include "stdio.h"
#include "panic.h"
#include "consts.h"
#include "defs.h"
#include "timer.h"

static inline paddr_t va_to_pa(const void *va) {
    return (paddr_t)((uint64_t)(uintptr_t)va - PAGE_OFFSET);
}

#define USER_STACK_TOP_VA  0x3FFFF000UL
#define USER_DYN_BASE      0x10000UL
#define INTERP_BASE        0x40000000UL
#define USER_TLS_VA        0x3E000000UL
#define TLS_TCB_SIZE       128
#define PTE_STACK          (PTE_V | PTE_R | PTE_W | PTE_X | PTE_U | PTE_A | PTE_D)

#define ASLR_BITS          8
#define ASLR_MASK          (((1UL << ASLR_BITS) - 1) << 16)
#define ASLR_MIN_ALIGN     (1UL << 16)

static uint64_t elf_aslr_bias(void) {
    uint64_t t = timer_get_ticks();
    uint64_t bits = ((t >> 3) ^ (t >> 17) ^ (t >> 37)) & ((1UL << ASLR_BITS) - 1);
    return bits << 16;
}

int elf_check_header(const Elf64_Ehdr *eh) {
    if (*(uint32_t *)eh->e_ident != ELF_MAGIC) return -ENOEXEC;
    if (eh->e_ident[4] != ELFCLASS64)           return -ENOEXEC;
    if (eh->e_ident[5] != ELFDATA2LSB)          return -ENOEXEC;
    if (eh->e_type != ET_EXEC && eh->e_type != ET_DYN) return -ENOEXEC;
    if (eh->e_phentsize < sizeof(Elf64_Phdr))   return -ENOEXEC;
    if (eh->e_phnum == 0 || eh->e_phnum > 64)  return -ENOEXEC;
    return 0;
}

static uint64_t seg_flags(uint32_t p_flags) {
    uint64_t f = PTE_V | PTE_U | PTE_A | PTE_D;
    if (p_flags & PF_R) f |= PTE_R;
    if (p_flags & PF_W) f |= PTE_W;
    if (p_flags & PF_X) f |= PTE_X;
    if (f & PTE_W) f |= PTE_R;
    return f;
}

static int map_segment(mm_struct_t *mm, uint64_t *pgdir, uint64_t va, uint64_t memsz,
                       const void *data, uint64_t filesz, uint64_t flags) {
    uint64_t start = va & ~(PAGE_SIZE - 1);
    uint64_t end   = ROUND_UP(va + memsz, PAGE_SIZE);

    for (uint64_t page = start; page < end; page += PAGE_SIZE) {
        void *frame = frame_alloc();
        if (!frame) return -ENOMEM;

        uint64_t *old_pte = pt_walk(pgdir, page, 0);
        if (old_pte && (*old_pte & PTE_V)) {
            paddr_t old_pa = SV39_PTE_ADDR(*old_pte);
            memcpy(frame, (void *)(old_pa + PAGE_OFFSET), PAGE_SIZE);
        }

        if (page < va + filesz) {
            uint64_t copy_off = (page < va) ? (va - page) : 0;
            uint64_t src_off  = (page < va) ? 0 : (page - va);
            uint64_t to_copy  = filesz - src_off;
            if (to_copy > PAGE_SIZE - copy_off)
                to_copy = PAGE_SIZE - copy_off;
            if (to_copy > 0 && data)
                memcpy((char *)frame + copy_off,
                       (const char *)data + src_off, to_copy);
        }

        int r = pt_map(pgdir, page, va_to_pa(frame), flags);
        if (r < 0) { frame_free(frame); return r; }
    }
    __asm__ volatile("sfence.vma" ::: "memory");

    if (mm) {
        vm_area_t *vma = kcalloc(1, sizeof(vm_area_t));
        if (vma) {
            vma->start = start;
            vma->end = end;
            vma->pte_flags = flags;
            vma->vm_flags = VM_ANON;
            if (flags & PTE_R) vma->vm_flags |= VM_READ;
            if (flags & PTE_W) vma->vm_flags |= VM_WRITE;
            if (flags & PTE_X) vma->vm_flags |= VM_EXEC;
            mm_insert_vma(mm, vma);
            mm->total_vm += (end - start) / PAGE_SIZE;
        }
    }
    return 0;
}

static int map_segment_from_fd(mm_struct_t *mm, uint64_t *pgdir, uint64_t va, uint64_t memsz,
                               int fd, long file_offset, uint64_t filesz, uint64_t flags) {
    uint64_t start = va & ~(PAGE_SIZE - 1);
    uint64_t end   = ROUND_UP(va + memsz, PAGE_SIZE);
    char *tmp = kmalloc(PAGE_SIZE);
    if (!tmp) return -ENOMEM;

    for (uint64_t page = start; page < end; page += PAGE_SIZE) {
        void *frame = frame_alloc();
        if (!frame) { kfree(tmp); return -ENOMEM; }

        uint64_t *old_pte = pt_walk(pgdir, page, 0);
        if (old_pte && (*old_pte & PTE_V)) {
            paddr_t old_pa = SV39_PTE_ADDR(*old_pte);
            memcpy(frame, (void *)(old_pa + PAGE_OFFSET), PAGE_SIZE);
        }

        if (page < va + filesz) {
            uint64_t copy_off = (page < va) ? (va - page) : 0;
            uint64_t src_off  = (page < va) ? 0 : (page - va);
            uint64_t to_copy  = filesz - src_off;
            if (to_copy > PAGE_SIZE - copy_off)
                to_copy = PAGE_SIZE - copy_off;
            if (to_copy > 0) {
                vfs_lseek(fd, file_offset + (long)src_off, SEEK_SET);
                int nr = vfs_read(fd, tmp, (size_t)to_copy);
                if (nr < 0) { frame_free(frame); kfree(tmp); return -EIO; }
                memcpy((char *)frame + copy_off, tmp, (size_t)nr);
            }
        }

        int r = pt_map(pgdir, page, va_to_pa(frame), flags);
        if (r < 0) { frame_free(frame); kfree(tmp); return r; }
    }
    kfree(tmp);
    __asm__ volatile("sfence.vma" ::: "memory");

    if (mm) {
        vm_area_t *vma = kcalloc(1, sizeof(vm_area_t));
        if (vma) {
            vma->start = start;
            vma->end = end;
            vma->pte_flags = flags;
            vma->vm_flags = VM_ANON;
            if (flags & PTE_R) vma->vm_flags |= VM_READ;
            if (flags & PTE_W) vma->vm_flags |= VM_WRITE;
            if (flags & PTE_X) vma->vm_flags |= VM_EXEC;
            mm_insert_vma(mm, vma);
            mm->total_vm += (end - start) / PAGE_SIZE;
        }
    }
    return 0;
}

static int map_stack(uint64_t *pgdir, uint64_t *stack_top_out) {
    for (int i = 0; i < INITIAL_STACK_PAGES; i++) {
        uint64_t va = USER_STACK_TOP_VA - (uint64_t)(INITIAL_STACK_PAGES - 1 - i) * PAGE_SIZE;
        void *frame = frame_alloc();
        if (!frame) return -ENOMEM;
        int r = pt_map(pgdir, va, va_to_pa(frame),
                        PTE_STACK);
        if (r < 0) { frame_free(frame); return r; }
    }
    *stack_top_out = USER_STACK_TOP_VA;
    __asm__ volatile("sfence.vma" ::: "memory");
    return 0;
}

static void *phys_for_va(uint64_t *pgdir, uint64_t va) {
    paddr_t pa = pt_translate(pgdir, va);
    if (pa == 0) return NULL;
    return (void *)((uint64_t)pa + PAGE_OFFSET);
}

static int setup_tls(uint64_t *pgdir, const void *tls_data, uint64_t tls_filesz,
                     uint64_t tls_memsz, uint64_t tls_align,
                     uint64_t *tls_va_out, uint64_t *tls_tp_out) {
    uint64_t tcb_offset = ROUND_UP(tls_memsz, tls_align);
    uint64_t total_size = tcb_offset + TLS_TCB_SIZE;
    uint64_t total_pages = ROUND_UP(total_size, PAGE_SIZE);

    for (uint64_t page = 0; page < total_pages; page++) {
        void *frame = frame_alloc();
        if (!frame) return -ENOMEM;
        int r = pt_map(pgdir, USER_TLS_VA + page * PAGE_SIZE,
                       va_to_pa(frame),
                       PTE_V | PTE_R | PTE_W | PTE_U | PTE_A | PTE_D);
        if (r < 0) { frame_free(frame); return r; }
    }

    uint64_t tls_va = USER_TLS_VA;
    if (tls_data && tls_filesz > 0) {
        for (uint64_t off = 0; off < tls_filesz; ) {
            uint64_t page_va = tls_va + off;
            void *dst = phys_for_va(pgdir, page_va);
            if (!dst) return -EFAULT;
            uint64_t chunk = tls_filesz - off;
            if (chunk > PAGE_SIZE - (page_va & (PAGE_SIZE - 1)))
                chunk = PAGE_SIZE - (page_va & (PAGE_SIZE - 1));
            memcpy(dst, (const char *)tls_data + off, chunk);
            off += chunk;
        }
    }

    uint64_t tcb_va = tls_va + tcb_offset;
    {
        void *dst = phys_for_va(pgdir, tcb_va);
        if (!dst) return -EFAULT;
        memset(dst, 0, TLS_TCB_SIZE);
        *(uint64_t *)dst = tcb_va;
        *((uint64_t *)dst + 1) = tcb_va;
    }

    *tls_va_out = tls_va;
    *tls_tp_out = tcb_va;
    __asm__ volatile("sfence.vma" ::: "memory");
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

    mm_struct_t mm;
    memset(&mm, 0, sizeof(mm));
    mm.pgdir = pgdir;
    mm.mmap_base = MMAP_BASE_ADDR;

    uint64_t load_bias = (eh->e_type == ET_DYN) ? (USER_DYN_BASE + elf_aslr_bias()) : 0;
    uint64_t base = 0, max_va = 0;
    const void *tls_data = NULL;
    uint64_t tls_filesz = 0, tls_memsz = 0, tls_align = 1;

    for (int i = 0; i < eh->e_phnum; i++) {
        if (eh->e_phoff + (i + 1) * eh->e_phentsize > len) continue;
        const Elf64_Phdr *ph = (const Elf64_Phdr *)
            ((const char *)buf + eh->e_phoff + i * eh->e_phentsize);
        if (ph->p_type == PT_TLS) {
            tls_data  = (const char *)buf + ph->p_offset;
            tls_filesz = ph->p_filesz;
            tls_memsz  = ph->p_memsz;
            tls_align  = ph->p_align < 1 ? 1 : ph->p_align;
            continue;
        }
        if (ph->p_type != PT_LOAD) continue;

        uint64_t seg_va = ph->p_vaddr + load_bias;
        const void *seg_data = (const char *)buf + ph->p_offset;

        r = map_segment(&mm, pgdir, seg_va, ph->p_memsz,
                         seg_data, ph->p_filesz, seg_flags(ph->p_flags));
        if (r < 0) { pt_destroy_user(pgdir); return r; }

        uint64_t seg_start = seg_va & ~(PAGE_SIZE - 1);
        uint64_t seg_end   = ROUND_UP(seg_va + ph->p_memsz, PAGE_SIZE);
        if (base == 0) base = seg_start;
        if (seg_end > max_va) max_va = seg_end;
    }

    uint64_t stack_top;
    r = map_stack(pgdir, &stack_top);
    if (r < 0) { pt_destroy_user(pgdir); return r; }

    uint64_t tls_va = 0, tls_tp = 0;
    if (tls_data && tls_filesz > 0) {
        r = setup_tls(pgdir, tls_data, tls_filesz, tls_memsz, tls_align, &tls_va, &tls_tp);
        if (r < 0) { pt_destroy_user(pgdir); return r; }
    }

    info->entry     = eh->e_entry + load_bias;
    info->base      = base;
    info->end_va    = max_va;
    info->brk       = ROUND_UP(max_va, PAGE_SIZE);
    info->phdr_va   = base + eh->e_phoff;
    info->phnum     = eh->e_phnum;
    info->phentsize = eh->e_phentsize;
    info->load_addr = base;
    info->load_size = (size_t)(max_va - base);
    info->pgdir     = pgdir;
    info->stack_top = stack_top;
    info->tls_va    = tls_va;
    info->tls_size  = tls_memsz;
    info->tls_tp    = tls_tp;
    info->interp_base = 0;
    info->mmap      = mm.mmap;

    return 0;
}

static int elf_load_interp(mm_struct_t *mm, uint64_t *pgdir, const char *path, uint64_t *entry_out, uint64_t *base_out) {
    int fd = vfs_open(path, O_RDONLY, 0);
    if (fd < 0) return fd;

    Elf64_Ehdr eh;
    if (vfs_lseek(fd, 0, SEEK_SET) < 0 || vfs_read(fd, (char *)&eh, sizeof(eh)) < (int)sizeof(eh)) {
        vfs_close(fd);
        return -ENOEXEC;
    }

    int r = elf_check_header(&eh);
    if (r < 0) { vfs_close(fd); return r; }

    Elf64_Phdr phdrs[64];
    int nph = eh.e_phnum < 64 ? eh.e_phnum : 64;
    vfs_lseek(fd, (long)eh.e_phoff, SEEK_SET);
    r = vfs_read(fd, (char *)phdrs, nph * sizeof(Elf64_Phdr));
    if (r < nph * (int)sizeof(Elf64_Phdr)) { vfs_close(fd); return -ENOEXEC; }

    uint64_t load_bias = INTERP_BASE + elf_aslr_bias();
    uint64_t base = 0, max_va = 0;

    for (int i = 0; i < nph; i++) {
        if (phdrs[i].p_type != PT_LOAD) continue;

        uint64_t seg_va = phdrs[i].p_vaddr + load_bias;
        r = map_segment_from_fd(mm, pgdir, seg_va, phdrs[i].p_memsz,
                                fd, (long)phdrs[i].p_offset,
                                phdrs[i].p_filesz, seg_flags(phdrs[i].p_flags));
        if (r < 0) { vfs_close(fd); return r; }

        uint64_t seg_start = seg_va & ~(PAGE_SIZE - 1);
        uint64_t seg_end   = ROUND_UP(seg_va + phdrs[i].p_memsz, PAGE_SIZE);
        if (base == 0) base = seg_start;
        if (seg_end > max_va) max_va = seg_end;
    }

    vfs_close(fd);
    *entry_out = eh.e_entry + load_bias;
    *base_out  = base;
    return 0;
}

int elf_load(int fd, const char *path, elf_load_info_t *info) {
    (void)path;
    Elf64_Ehdr eh;
    if (vfs_lseek(fd, 0, SEEK_SET) < 0) return -EIO;
    int r = vfs_read(fd, (char *)&eh, sizeof(eh));
    if (r < (int)sizeof(eh)) return -ENOEXEC;
    r = elf_check_header(&eh);
    if (r < 0) goto fail_early;

    Elf64_Phdr phdrs[64];
    int nph = eh.e_phnum < 64 ? eh.e_phnum : 64;
    vfs_lseek(fd, (long)eh.e_phoff, SEEK_SET);
    r = vfs_read(fd, (char *)phdrs, nph * sizeof(Elf64_Phdr));
    if (r < nph * (int)sizeof(Elf64_Phdr)) { r = -ENOEXEC; goto fail_early; }

    uint64_t *pgdir = pt_create();
    if (!pgdir) { r = -ENOMEM; goto fail_early; }
    pt_map_kernel(pgdir);

    mm_struct_t mm;
    memset(&mm, 0, sizeof(mm));
    mm.pgdir = pgdir;
    mm.mmap_base = MMAP_BASE_ADDR;

    uint64_t load_bias = (eh.e_type == ET_DYN) ? (USER_DYN_BASE + elf_aslr_bias()) : 0;
    uint64_t base = 0, max_va = 0, head_va = 0;
    void *tls_data = NULL;
    uint64_t tls_filesz = 0, tls_memsz = 0, tls_align = 1;

    int has_interp = 0;
    char interp_path[MAX_PATH_LEN];

    for (int i = 0; i < nph; i++) {
        if (phdrs[i].p_type == PT_INTERP) {
            has_interp = 1;
            vfs_lseek(fd, (long)phdrs[i].p_offset, SEEK_SET);
            int len = phdrs[i].p_filesz < MAX_PATH_LEN ? (int)phdrs[i].p_filesz : MAX_PATH_LEN - 1;
            vfs_read(fd, interp_path, (size_t)len);
            interp_path[len] = '\0';
            continue;
        }
        if (phdrs[i].p_type == PT_TLS) {
            tls_filesz = phdrs[i].p_filesz;
            tls_memsz  = phdrs[i].p_memsz;
            tls_align  = phdrs[i].p_align < 1 ? 1 : phdrs[i].p_align;
            if (tls_filesz > 0) {
                tls_data = kmalloc((size_t)tls_filesz);
                if (!tls_data) { r = -ENOMEM; goto fail_elf; }
                vfs_lseek(fd, (long)phdrs[i].p_offset, SEEK_SET);
                int nr = vfs_read(fd, (char *)tls_data, (size_t)tls_filesz);
                if (nr < 0) { kfree(tls_data); tls_data = NULL; r = -EIO; goto fail_elf; }
            }
            continue;
        }
        if (phdrs[i].p_type != PT_LOAD) continue;

        uint64_t seg_va = phdrs[i].p_vaddr + load_bias;
        uint64_t seg_flags_val = seg_flags(phdrs[i].p_flags);
        r = map_segment_from_fd(&mm, pgdir, seg_va, phdrs[i].p_memsz,
                                fd, (long)phdrs[i].p_offset,
                                phdrs[i].p_filesz, seg_flags_val);
        if (r < 0) { goto fail_elf; }

        uint64_t seg_start = seg_va & ~(PAGE_SIZE - 1);
        uint64_t seg_end   = ROUND_UP(seg_va + phdrs[i].p_memsz, PAGE_SIZE);
        if (phdrs[i].p_offset == 0) head_va = phdrs[i].p_vaddr + load_bias;
        if (base == 0) base = seg_start;
        if (seg_end > max_va) max_va = seg_end;
    }

    uint64_t interp_entry = 0;
    uint64_t interp_base  = 0;
    if (has_interp) {
        int interp_fd = vfs_open(interp_path, O_RDONLY, 0);
        if (interp_fd >= 0) {
            vfs_close(interp_fd);
        } else {
            r = -ENOENT;
            goto fail_elf;
        }
        r = elf_load_interp(&mm, pgdir, interp_path, &interp_entry, &interp_base);
        if (r < 0) goto fail_elf;
    }

    uint64_t stack_top;
    r = map_stack(pgdir, &stack_top);
    if (r < 0) { goto fail_elf; }

    info->entry      = has_interp ? interp_entry : (eh.e_entry + load_bias);
    info->exec_entry = eh.e_entry + load_bias;
    info->base       = base;
    info->end_va     = max_va;
    info->brk        = ROUND_UP(max_va, PAGE_SIZE);
    info->phdr_va    = head_va ? (head_va + eh.e_phoff) : (base + eh.e_phoff);
    info->phnum      = (uint32_t)nph;
    info->phentsize  = eh.e_phentsize;
    info->load_addr  = base;
    info->load_size  = (size_t)(max_va - base);
    uint64_t tls_va = 0, tls_tp = 0;
    if (tls_data && tls_filesz > 0) {
        r = setup_tls(pgdir, tls_data, tls_filesz, tls_memsz, tls_align, &tls_va, &tls_tp);
        if (r < 0) { kfree(tls_data); pt_destroy_user(pgdir); return r; }
    }

    info->pgdir      = pgdir;
    info->stack_top  = stack_top;
    info->tls_va     = tls_va;
    info->tls_size   = tls_memsz;
    info->tls_tp     = tls_tp;
    info->interp_base = interp_base;
    info->mmap       = mm.mmap;

    kfree(tls_data);
    return 0;
fail_elf:
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
fail_early:
    return r;
}

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

    const char *platform = "riscv64";
    int plat_len = 8;
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
        for (int i = 0; i < 16; i++) ((uint8_t *)dst)[i] = (uint8_t)(i ^ 0xAA);
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
    sp_va -= naux * 2 * 8;
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

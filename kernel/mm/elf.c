/*
 * A20OS ELF64 Loader with per-process SV39 page tables.
 *
 * Each loaded binary gets its own page table with:
 *   - User segments mapped at their ELF virtual addresses (PTE_USER)
 *   - Kernel space identity-mapped (PTE_KERN, no PTE_U)
 *   - MMIO devices identity-mapped (PTE_KERN, no PTE_U)
 *   - User stack mapped at VA 0x7FFFF000 downward
 */

#include "elf.h"
#include "mm.h"
#include "vfs.h"
#include "string.h"
#include "stdio.h"
#include "panic.h"
#include "consts.h"
#include "defs.h"

#define USER_STACK_TOP_VA  0x7FFFF000UL
#define USER_STACK_PAGES   16
#define USER_DYN_BASE      0x10000UL

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
    return f;
}

static int map_segment(uint64_t *pgdir, uint64_t va, uint64_t memsz,
                       const void *data, uint64_t filesz, uint64_t flags) {
    uint64_t start = va & ~(PAGE_SIZE - 1);
    uint64_t end   = ROUND_UP(va + memsz, PAGE_SIZE);

    for (uint64_t page = start; page < end; page += PAGE_SIZE) {
        void *frame = frame_alloc();
        if (!frame) return -ENOMEM;

        memset(frame, 0, PAGE_SIZE);

        uint64_t off_in_seg = page - start;
        if (off_in_seg < filesz) {
            uint64_t copy_off = (page < va) ? (va - page) : 0;
            uint64_t src_off  = (page < va) ? 0 : (page - va);
            uint64_t to_copy  = filesz - src_off;
            if (to_copy > PAGE_SIZE - copy_off)
                to_copy = PAGE_SIZE - copy_off;
            if (to_copy > 0 && data)
                memcpy((char *)frame + copy_off,
                       (const char *)data + src_off, to_copy);
        }

        int r = pt_map(pgdir, page, (paddr_t)(uintptr_t)frame, flags);
        if (r < 0) { frame_free(frame); return r; }
    }
    return 0;
}

static int map_stack(uint64_t *pgdir, uint64_t *stack_top_out) {
    for (int i = 0; i < USER_STACK_PAGES; i++) {
        uint64_t va = USER_STACK_TOP_VA - (uint64_t)(USER_STACK_PAGES - 1 - i) * PAGE_SIZE;
        void *frame = frame_alloc();
        if (!frame) return -ENOMEM;
        memset(frame, 0, PAGE_SIZE);
        int r = pt_map(pgdir, va, (paddr_t)(uintptr_t)frame,
                        PTE_V | PTE_R | PTE_W | PTE_U | PTE_A | PTE_D);
        if (r < 0) { frame_free(frame); return r; }
    }
    *stack_top_out = USER_STACK_TOP_VA;
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

    uint64_t load_bias = (eh->e_type == ET_DYN) ? USER_DYN_BASE : 0;
    uint64_t base = 0, max_va = 0;

    for (int i = 0; i < eh->e_phnum; i++) {
        if (eh->e_phoff + (i + 1) * eh->e_phentsize > len) continue;
        const Elf64_Phdr *ph = (const Elf64_Phdr *)
            ((const char *)buf + eh->e_phoff + i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD) continue;

        uint64_t seg_va = ph->p_vaddr + load_bias;
        const void *seg_data = (const char *)buf + ph->p_offset;

        r = map_segment(pgdir, seg_va, ph->p_memsz,
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

    info->entry     = eh->e_entry + load_bias;
    info->base      = base;
    info->end_va    = max_va;
    info->brk       = ROUND_UP(max_va, PAGE_SIZE);
    info->phdr_va   = eh->e_phoff + load_bias;
    info->phnum     = eh->e_phnum;
    info->phentsize = eh->e_phentsize;
    info->load_addr = base;
    info->load_size = (size_t)(max_va - base);
    info->pgdir     = pgdir;
    info->stack_top = stack_top;

    printf("[ELF] buf load: entry=0x%lx base=0x%lx brk=0x%lx sp=0x%lx pgdir=0x%lx\n",
           (unsigned long)info->entry, (unsigned long)info->base,
           (unsigned long)info->brk, (unsigned long)info->stack_top,
           (unsigned long)(uintptr_t)pgdir);
    return 0;
}

int elf_load(int fd, elf_load_info_t *info) {
    Elf64_Ehdr eh;
    if (vfs_lseek(fd, 0, SEEK_SET) < 0) return -EIO;
    int r = vfs_read(fd, (char *)&eh, sizeof(eh));
    if (r < (int)sizeof(eh)) return -ENOEXEC;

    r = elf_check_header(&eh);
    if (r < 0) return r;

    Elf64_Phdr phdrs[64];
    int nph = eh.e_phnum < 64 ? eh.e_phnum : 64;
    vfs_lseek(fd, (long)eh.e_phoff, SEEK_SET);
    r = vfs_read(fd, (char *)phdrs, nph * sizeof(Elf64_Phdr));
    if (r < nph * (int)sizeof(Elf64_Phdr)) return -ENOEXEC;

    uint64_t *pgdir = pt_create();
    if (!pgdir) return -ENOMEM;
    pt_map_kernel(pgdir);

    uint64_t load_bias = (eh.e_type == ET_DYN) ? USER_DYN_BASE : 0;
    uint64_t base = 0, max_va = 0;

    for (int i = 0; i < nph; i++) {
        if (phdrs[i].p_type != PT_LOAD) continue;

        uint64_t seg_va = phdrs[i].p_vaddr + load_bias;

        void *seg_buf = NULL;
        if (phdrs[i].p_filesz > 0) {
            seg_buf = kmalloc((size_t)phdrs[i].p_filesz);
            if (!seg_buf) { pt_destroy_user(pgdir); return -ENOMEM; }
            vfs_lseek(fd, (long)phdrs[i].p_offset, SEEK_SET);
            int nr = vfs_read(fd, (char *)seg_buf, (size_t)phdrs[i].p_filesz);
            if (nr < 0) { kfree(seg_buf); pt_destroy_user(pgdir); return -EIO; }
        }

        r = map_segment(pgdir, seg_va, phdrs[i].p_memsz,
                         seg_buf, phdrs[i].p_filesz, seg_flags(phdrs[i].p_flags));
        kfree(seg_buf);
        if (r < 0) { pt_destroy_user(pgdir); return r; }

        uint64_t seg_start = seg_va & ~(PAGE_SIZE - 1);
        uint64_t seg_end   = ROUND_UP(seg_va + phdrs[i].p_memsz, PAGE_SIZE);
        if (base == 0) base = seg_start;
        if (seg_end > max_va) max_va = seg_end;
    }

    uint64_t stack_top;
    r = map_stack(pgdir, &stack_top);
    if (r < 0) { pt_destroy_user(pgdir); return r; }

    info->entry     = eh.e_entry + load_bias;
    info->base      = base;
    info->end_va    = max_va;
    info->brk       = ROUND_UP(max_va, PAGE_SIZE);
    info->phdr_va   = eh.e_phoff + load_bias;
    info->phnum     = (uint32_t)nph;
    info->phentsize = eh.e_phentsize;
    info->load_addr = base;
    info->load_size = (size_t)(max_va - base);
    info->pgdir     = pgdir;
    info->stack_top = stack_top;

    return 0;
}

#define AT_NULL     0
#define AT_PHDR     3
#define AT_PHENT    4
#define AT_PHNUM    5
#define AT_PAGESZ   6
#define AT_BASE     7
#define AT_FLAGS    8
#define AT_ENTRY    9
#define AT_RANDOM   25

static void *phys_for_va(uint64_t *pgdir, uint64_t va) {
    paddr_t pa = pt_translate(pgdir, va);
    if (pa == 0) return NULL;
    return (void *)(uintptr_t)pa;
}

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

    uint64_t auxv[][2] = {
        { AT_PHDR,   info->phdr_va  },
        { AT_PHENT,  info->phentsize },
        { AT_PHNUM,  info->phnum     },
        { AT_PAGESZ, PAGE_SIZE       },
        { AT_BASE,   info->base      },
        { AT_FLAGS,  0               },
        { AT_ENTRY,  info->entry     },
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

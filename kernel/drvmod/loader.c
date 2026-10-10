/*
 * A20OS kernel driver modules — ELF loader (multi-architecture).
 *
 * Loads a driver module (ELF relocatable object, ET_REL) into the kernel
 * direct map and resolves its external symbols against the driver
 * framework export table ONLY (symbol whitelist).  Instruction encodings,
 * veneers, and paired relocations are supplied by kernel/arch/common.
 *
 * Every pointer derived from the module file (section table, symbol table,
 * string tables, relocation entries) is bounds-checked against the file
 * buffer before use; a malformed module is rejected instead of corrupting
 * the kernel heap.
 */

#include "drvmod/drvmod.h"
#include "drvmod/reloc.h"
#include "drvmod/export.h"

#include "core/klog.h"
#include "core/stdio.h"
#include "core/string.h"
#include "core/panic.h"
#include "fs/vfs.h"
#include "mm/frame.h"
#include "mm/mm.h"
#include "mm/slab.h"

#define DRV_MOD_MAX_MODULES  32

#define ELFCLASS64 2
#define ELFDATA2LSB 1
#define SHN_XINDEX 0xFFFF

typedef struct drv_module {
    int used;
    int id;
    char name[DRV_MOD_MAX_NAME];
    uintptr_t base;         /* load address */
    uintptr_t entry;        /* DriverEntry address */
    int pinned;             /* set after DriverEntry registers a driver */
    size_t total_size;
    pfn_t alloc_pfn;
    uint32_t alloc_order;
} drv_module_t;

static drv_module_t drv_modules[DRV_MOD_MAX_MODULES];

#define SHT_PROGBITS 1
#define SHT_SYMTAB 2
#define SHT_NOBITS 8
#define SHT_RELA   4
#define SHF_ALLOC     0x2
#define SHF_EXECINSTR 0x4

static void *drvmod_resolve_symbol(const char *name)
{
    for (unsigned i = 0; i < drv_export_count; i++) {
        if (strcmp(drv_export_table[i].name, name) == 0)
            return drv_export_table[i].addr;
    }
    for (unsigned i = 0; i < arch_drv_export_count; i++) {
        if (strcmp(arch_drv_export_table[i].name, name) == 0)
            return arch_drv_export_table[i].addr;
    }
    return NULL;
}

/* ---- bounds helpers (all offsets are module-file or load-layout offsets) ---- */

static int drvmod_range_ok64(uint64_t off, uint64_t len, uint64_t cap)
{
    return off <= cap && len <= cap - off;
}

static int drvmod_range_ok32(uint32_t off, uint32_t len, uint32_t cap)
{
    return off <= cap && len <= cap - off;
}

/* Symbol name, validated against the string table size. */
static const char *drvmod_sym_name(const elf_sym_t *sym, const char *strtab,
                                   uint32_t strtab_size)
{
    if (!strtab || sym->st_name >= strtab_size)
        return NULL;
    return strtab + sym->st_name;
}

#define DRV_MOD_MAX_PLACED 8
#define DRV_MOD_MAX_RELA_SETS 8

static int drvmod_sec_lookup(const drvmod_secmap_t *map, uint32_t nmap,
                             uint32_t idx, uint32_t *load_off,
                             uint32_t *size)
{
    for (uint32_t i = 0; i < nmap; i++) {
        if (map[i].idx == idx) {
            if (load_off)
                *load_off = map[i].load_off;
            if (size)
                *size = map[i].size;
            return 0;
        }
    }
    return -1;
}

static void drvmod_free_pages(pfn_t pfn, uint32_t order)
{
    if (pfn != PFN_NONE)
        pfa_free(pfn, (int)order);
}

/* Optional architecture-owned allocator for memory outside the frame pool.
 * Return 1 when allocated, 0 to use the frame allocator, or a negative error. */
__attribute__((weak)) int arch_drvmod_alloc_reserved(uint32_t order,
                                                       uintptr_t *addr_out)
{
    (void)order;
    (void)addr_out;
    return 0;
}

__attribute__((weak)) void arch_drvmod_free_reserved(uintptr_t addr,
                                                       uint32_t order)
{
    (void)addr;
    (void)order;
}

static void drvmod_free_image(uintptr_t base, pfn_t pfn, uint32_t order)
{
    if (pfn == PFN_NONE)
        arch_drvmod_free_reserved(base, order);
    else
        drvmod_free_pages(pfn, order);
}

/* Resolve a relocation symbol to its final address.  Returns 0 on success;
 * on failure *err is set and the result must not be used. */
int drvmod_sym_addr(const elf_sym_t *sym, const char *strtab,
                           uint32_t strtab_size, const drvmod_secmap_t *map,
                           uint32_t nmap, uintptr_t load_base,
                           uintptr_t *out)
{
    if (sym->st_shndx == 0) {
        /* External symbol: framework whitelist only. */
        const char *nm = drvmod_sym_name(sym, strtab, strtab_size);
        void *addr = nm ? drvmod_resolve_symbol(nm) : NULL;
        if (!addr) {
            printf("[DRVMOD] unresolved symbol '%s'\n", nm ? nm : "?");
            return -1;
        }
        *out = (uintptr_t)addr;
        return 0;
    }
    uint32_t soff, sec_size;
    if (drvmod_sec_lookup(map, nmap, sym->st_shndx, &soff, &sec_size) < 0) {
        /* Symbol in a section we do not place (e.g. .debug): it has no
         * runtime address.  Referencing it is a module error. */
        printf("[DRVMOD] symbol in unplaced section %u\n", sym->st_shndx);
        return -1;
    }
    if (sym->st_value >= sec_size) {
        printf("[DRVMOD] symbol value 0x%lx outside section %u\n",
             (unsigned long)sym->st_value, sym->st_shndx);
        return -1;
    }
    *out = load_base + soff + sym->st_value;
    return 0;
}


int drvmod_load(int fd, const char *name)
{
    if (fd < 0 || !name)
        return -EINVAL;

    int slot = -1;
    for (int i = 0; i < DRV_MOD_MAX_MODULES; i++) {
        if (!drv_modules[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0)
        return -ENOSPC;

    /* Read buffer from the frame pool, not kmalloc: on some machines
     * (loongarch64) a kmalloc'd slab for 256 KiB draws from the same
     * buddy blocks the module's pfa_alloc later hands out, so the module
     * image can land on top of this buffer; kfree(buf) would then return
     * the module's own pages to the allocator and the next module (or any
     * DMA) overwrites the first one's GOT/rodata. */
    /* The caller may have read the descriptor (or anything else) first;
     * always read the module image from the start. */
    if (vfs_lseek(fd, 0, SEEK_SET) < 0)
        return -EIO;

    pfn_t buf_pfn = pfa_alloc(DRV_MOD_BUF_ORDER);
    printf("[DRVMOD] %s: buf phys=0x%lx order=%d\n", name,
           (unsigned long)pfn_to_phys(buf_pfn), DRV_MOD_BUF_ORDER);
    if (buf_pfn == PFN_NONE) {
        printf("[DRVMOD] %s: pfa_alloc buf failed\n", name);
        return -ENOMEM;
    }
    char *buf = (char *)(PAGE_OFFSET + pfn_to_phys(buf_pfn));

    size_t got = 0;
    while (got < DRV_MOD_MAX_SIZE) {
        int64_t n = vfs_read(fd, buf + got, DRV_MOD_MAX_SIZE - got);
        if (n < 0) {
            drvmod_free_pages(buf_pfn, DRV_MOD_BUF_ORDER);
            return (int)n;
        }
        if (n == 0)
            break;
        got += (size_t)n;
    }
    if (got < sizeof(elf_ehdr_t)) {
        drvmod_free_pages(buf_pfn, DRV_MOD_BUF_ORDER);
        return -ENOEXEC;
    }

    elf_ehdr_t *eh = (elf_ehdr_t *)buf;

    /* ---- file-level identity checks ---- */
    if (memcmp(eh->e_ident, "\x7f" "ELF", 4) != 0 ||
        eh->e_ident[4] != ELFCLASS64 || eh->e_ident[5] != ELFDATA2LSB ||
        eh->e_type != 1 /* ET_REL */ || eh->e_version != 1) {
        drvmod_free_pages(buf_pfn, DRV_MOD_BUF_ORDER);
        return -ENOEXEC;
    }
    uint32_t machine = eh->e_machine;
    const arch_module_format_t *format = arch_module_format(machine);
    if (machine != ARCH_ELF_MACHINE || !format) {
        kerr("[DRVMOD] %s: unsupported machine %u\n", name, machine);
        drvmod_free_pages(buf_pfn, DRV_MOD_BUF_ORDER);
        return -ENOEXEC;
    }
    if (eh->e_shnum == 0 || eh->e_shnum >= SHN_XINDEX) {
        /* Extended section numbering is not supported. */
        drvmod_free_pages(buf_pfn, DRV_MOD_BUF_ORDER);
        return -ENOEXEC;
    }

    uint32_t nshdrs = eh->e_shnum;
    if (eh->e_shentsize != sizeof(elf_shdr_t) ||
        !drvmod_range_ok64(eh->e_shoff,
                           (uint64_t)nshdrs * sizeof(elf_shdr_t), got)) {
        drvmod_free_pages(buf_pfn, DRV_MOD_BUF_ORDER);
        return -ENOEXEC;
    }
    elf_shdr_t *shdrs = (elf_shdr_t *)(buf + eh->e_shoff);

    if (eh->e_shstrndx >= nshdrs) {
        drvmod_free_pages(buf_pfn, DRV_MOD_BUF_ORDER);
        return -ENOEXEC;
    }
    elf_shdr_t *shstr_sh = &shdrs[eh->e_shstrndx];
    if (!drvmod_range_ok64(shstr_sh->sh_offset, shstr_sh->sh_size, got)) {
        kerr("[DRVMOD] %s: shstr escapes file\n", name);
        drvmod_free_pages(buf_pfn, DRV_MOD_BUF_ORDER);
        return -ENOEXEC;
    }
    const char *shstr = (const char *)(buf + shstr_sh->sh_offset);
    uint32_t shstr_size = (uint32_t)shstr_sh->sh_size;
    const a20_driver_descriptor_t *descriptor = NULL;

    /* ---- per-section validity: every section we touch must be entirely
     * within the file buffer, and every name lookup inside the section
     * name table must stay in bounds ---- */
    for (uint32_t i = 0; i < nshdrs; i++) {
        elf_shdr_t *sh = &shdrs[i];
        if (sh->sh_name >= shstr_size) {
            kerr("[DRVMOD] %s: bad section name offset %u\n", name,
                 sh->sh_name);
            drvmod_free_pages(buf_pfn, DRV_MOD_BUF_ORDER);
            return -ENOEXEC;
        }
        switch (sh->sh_type) {
        case SHT_PROGBITS:
        case SHT_SYMTAB:
        case SHT_RELA:
            if (!drvmod_range_ok64(sh->sh_offset, sh->sh_size, got)) {
                kerr("[DRVMOD] %s: section %u escapes file\n", name, i);
                drvmod_free_pages(buf_pfn, DRV_MOD_BUF_ORDER);
                return -ENOEXEC;
            }
            break;
        default:
            break;
        }
        if (strcmp(shstr + sh->sh_name, ".a20drv") == 0) {
            if (sh->sh_type != SHT_PROGBITS ||
                sh->sh_size != sizeof(*descriptor)) {
                kerr("[DRVMOD] %s: bad .a20drv section type=%u size=%llu want=%lu\n",
                     name, sh->sh_type, (unsigned long long)sh->sh_size,
                     sizeof(*descriptor));
                drvmod_free_pages(buf_pfn, DRV_MOD_BUF_ORDER);
                return -ENOEXEC;
            }
            descriptor = (const a20_driver_descriptor_t *)(buf + sh->sh_offset);
        }
    }

    /* ABI is reported apart from the rest so the two failures stay
     * distinguishable: a magic/version failure means the file is not a driver
     * package or is corrupt, an ABI failure means it is a driver package built
     * for a different kernel and has to be rebuilt.  Without the check the
     * kernel reads vtable fields past the end of a module built against an
     * older net_dev_ops_t and calls them. */
    if (descriptor && descriptor->abi != A20_DRIVER_ABI &&
        descriptor->magic == A20_DRIVER_DESCRIPTOR_MAGIC) {
        kerr("[DRVMOD] %s: driver ABI %u, kernel expects %u -- rebuild the "
             "module against this kernel\n",
             name, descriptor->abi, A20_DRIVER_ABI);
        drvmod_free_pages(buf_pfn, DRV_MOD_BUF_ORDER);
        return -ENOEXEC;
    }

    if (!descriptor || !a20_driver_descriptor_sane(descriptor) ||
        descriptor->placement != A20_DRIVER_PLACEMENT_KERNEL_MODULE ||
        descriptor->type < A20_DRIVER_TYPE_RTC ||
        descriptor->type > A20_DRIVER_TYPE_CHAR) {
        kerr("[DRVMOD] %s: missing or invalid kernel driver descriptor\n", name);
        drvmod_free_pages(buf_pfn, DRV_MOD_BUF_ORDER);
        return -ENOEXEC;
    }

    /* Locate sections and compute the shadow layout. Every loadable section
     * gets its own aligned region; in particular .data must not overlap
     * .rodata strings. */
    uint32_t text_size = 0;
    uint32_t total_size = 0;
    elf_sym_t *syms = NULL;
    uint32_t nsyms = 0;
    const char *strtab = NULL;
    uint32_t strtab_size = 0;
    /* Every RELA section is applied against its own target section (the
     * .rela.text/.rela.data/.rela.rodata split must not be collapsed by
     * substring matching: ".rodata" contains ".data"). */
    typedef struct {
        uint32_t target_sec;
        elf_rela_t *rela;
        uint32_t nrela;
    } drvmod_rela_set_t;
    drvmod_rela_set_t rela_sets[DRV_MOD_MAX_RELA_SETS];
    uint32_t nrela_sets = 0;
    elf_rela_t *rela_text = NULL;
    uint32_t nrela_text = 0;
    uint32_t rela_text_off = 0, rela_text_size = 0;
    drvmod_secmap_t secmap[DRV_MOD_MAX_PLACED];
    uint32_t nmap = 0;

    /* Layout: [.text][veneers] page-aligned, then data/bss, then the GOT.
     * The text+veneer region is remapped read-only/executable after the
     * copy below (arch_kwx_module_protect, kernel W^X); data and the GOT
     * stay writable and non-executable.  Offsets for data sections are
     * assigned in the placement pass after the veneer count is known. */
    for (uint32_t i = 0; i < nshdrs; i++) {
        elf_shdr_t *sh = &shdrs[i];
        const char *n = shstr + sh->sh_name;
        if (strcmp(n, ".text") == 0) {
            if (sh->sh_type != SHT_PROGBITS) {
                kerr("[DRVMOD] %s: .text type=%u\n", name, sh->sh_type);
                drvmod_free_pages(buf_pfn, DRV_MOD_BUF_ORDER);
                return -ENOEXEC;
            }
            if (nmap >= DRV_MOD_MAX_PLACED) {
                kerr("[DRVMOD] %s: too many loadable sections\n", name);
                drvmod_free_pages(buf_pfn, DRV_MOD_BUF_ORDER);
                return -ENOEXEC;
            }
            text_size = (uint32_t)sh->sh_size;
            secmap[nmap].idx = i;
            secmap[nmap].load_off = 0;
            secmap[nmap].size = (uint32_t)sh->sh_size;
            nmap++;
        } else if ((sh->sh_flags & SHF_ALLOC) &&
                   !(sh->sh_flags & SHF_EXECINSTR) &&
                   (sh->sh_type == SHT_PROGBITS ||
                    sh->sh_type == SHT_NOBITS) &&
                   strcmp(n, ".a20drv") != 0 &&
                   strcmp(n, ".eh_frame") != 0) {
            if (nmap >= DRV_MOD_MAX_PLACED) {
                kerr("[DRVMOD] %s: too many loadable sections\n", name);
                drvmod_free_pages(buf_pfn, DRV_MOD_BUF_ORDER);
                return -ENOEXEC;
            }
            secmap[nmap].idx = i;
            secmap[nmap].load_off = 0;  /* assigned by the placement pass */
            secmap[nmap].size = (uint32_t)sh->sh_size;
            nmap++;
        } else if (sh->sh_type == SHT_SYMTAB) {
            if (sh->sh_size % sizeof(elf_sym_t) != 0 ||
                sh->sh_link >= nshdrs) {
                kerr("[DRVMOD] %s: bad symtab size=%llu link=%u\n", name,
                     (unsigned long long)sh->sh_size, sh->sh_link);
                drvmod_free_pages(buf_pfn, DRV_MOD_BUF_ORDER);
                return -ENOEXEC;
            }
            syms = (elf_sym_t *)(buf + sh->sh_offset);
            nsyms = (uint32_t)(sh->sh_size / sizeof(elf_sym_t));
            elf_shdr_t *link_sh = &shdrs[sh->sh_link];
            if (!drvmod_range_ok64(link_sh->sh_offset, link_sh->sh_size,
                                   got)) {
                drvmod_free_pages(buf_pfn, DRV_MOD_BUF_ORDER);
                return -ENOEXEC;
            }
            strtab = (const char *)(buf + link_sh->sh_offset);
            strtab_size = (uint32_t)link_sh->sh_size;
        } else if (sh->sh_type == SHT_RELA) {
            if (sh->sh_size % sizeof(elf_rela_t) != 0 ||
                sh->sh_info >= nshdrs) {
                kerr("[DRVMOD] %s: bad rela\n", name);
                drvmod_free_pages(buf_pfn, DRV_MOD_BUF_ORDER);
                return -ENOEXEC;
            }
            if (nrela_sets >= DRV_MOD_MAX_RELA_SETS) {
                kerr("[DRVMOD] %s: too many rela sections\n", name);
                drvmod_free_pages(buf_pfn, DRV_MOD_BUF_ORDER);
                return -ENOEXEC;
            }
            rela_sets[nrela_sets].target_sec = sh->sh_info;
            rela_sets[nrela_sets].rela =
                (elf_rela_t *)(buf + sh->sh_offset);
            rela_sets[nrela_sets].nrela =
                (uint32_t)(sh->sh_size / sizeof(elf_rela_t));
            nrela_sets++;
        }
    }
    if (text_size == 0 || !syms || !strtab) {
        drvmod_free_pages(buf_pfn, DRV_MOD_BUF_ORDER);
        return -ENOEXEC;
    }
    for (uint32_t i = 0; i < nrela_sets; i++) {
        uint32_t tidx = rela_sets[i].target_sec;
        if (tidx >= nshdrs)
            continue;
        const char *tn = shstr + shdrs[tidx].sh_name;
        if (strcmp(tn, ".text") == 0) {
            if (rela_text) {
                kerr("[DRVMOD] %s: multiple .text relocation sections\n",
                     name);
                drvmod_free_pages(buf_pfn, DRV_MOD_BUF_ORDER);
                return -ENOEXEC;
            }
            rela_text = rela_sets[i].rela;
            nrela_text = rela_sets[i].nrela;
            if (drvmod_sec_lookup(secmap, nmap, tidx, &rela_text_off,
                                  &rela_text_size) < 0)
                rela_text_off = rela_text_size = 0;
        }
    }

    /* AArch64 and RISC-V external calls may need a veneer.  Veneers are
     * executable, so they live immediately after .text inside the RX region
     * (kernel W^X), not at the module tail.  Reserve one for every external
     * call relocation before the final load address is known; relocation
     * only uses it when the direct displacement is out of range. */
    uint32_t *veneer_off = NULL;
    uint32_t nveneers = 0;
    uint32_t veneer_size = format->veneer_size;
    uint32_t veneer_align = format->veneer_align;
    uint32_t veneer_base = 0;
    if (format->veneer_size && nrela_text) {
        for (uint32_t i = 0; i < nrela_text; i++) {
            uint32_t type = ELF_R_TYPE(rela_text[i].r_info);
            int is_call = type == format->call_types[0] ||
                type == format->call_types[1];
            if (!is_call)
                continue;
            uint32_t symidx = ELF_R_SYM(rela_text[i].r_info);
            if (symidx >= nsyms)
                continue;
            if (syms[symidx].st_shndx != 0)
                continue;   /* module-internal: direct branch */
            nveneers++;
        }
        if (nveneers) {
            veneer_base = (text_size + veneer_align - 1) & ~(veneer_align - 1);
            if (veneer_base + nveneers * veneer_size > DRV_MOD_MAX_SIZE) {
                drvmod_free_pages(buf_pfn, DRV_MOD_BUF_ORDER);
                return -ENOEXEC;
            }
            veneer_off = kmalloc(nrela_text * sizeof(uint32_t));
            if (!veneer_off) {
                drvmod_free_pages(buf_pfn, DRV_MOD_BUF_ORDER);
                return -ENOMEM;
            }
            memset(veneer_off, 0, nrela_text * sizeof(uint32_t));
            uint32_t voff = veneer_base;
            for (uint32_t i = 0; i < nrela_text; i++) {
                uint32_t type = ELF_R_TYPE(rela_text[i].r_info);
                int is_call = type == format->call_types[0] ||
                type == format->call_types[1];
                if (!is_call)
                    continue;
                uint32_t symidx = ELF_R_SYM(rela_text[i].r_info);
                if (symidx >= nsyms || syms[symidx].st_shndx != 0)
                    continue;
                veneer_off[i] = voff;
                voff += veneer_size;
            }
        }
    }

    /* Placement pass: the RX text region is page-aligned apart from the
     * writable data area so the post-load permission split never shares a
     * page between code and data.  Veneers (riscv64/aarch64 only) sit right
     * after .text inside the RX region; on machines without veneers the
     * region still must cover .text itself. */
    uint32_t text_end = veneer_base + nveneers * veneer_size;
    if (text_end < text_size)
        text_end = text_size;
    uint32_t text_region_size =
        (text_end + PAGE_SIZE - 1) & ~(uint32_t)(PAGE_SIZE - 1);
    total_size = text_region_size;
    for (uint32_t k = 0; k < nmap; k++) {
        elf_shdr_t *sh = &shdrs[secmap[k].idx];
        const char *n = shstr + sh->sh_name;
        if (strcmp(n, ".text") == 0)
            continue;
        uint32_t align = (uint32_t)sh->sh_addralign;
        if (align < 4)
            align = 4;
        total_size = (total_size + align - 1) & ~(align - 1);
        secmap[k].load_off = total_size;
        total_size += secmap[k].size;
    }

    /* RISC-V and LoongArch64 external data references load the symbol address
     * through a module-local GOT. Allocate one 8-byte slot per HI relocation
     * at the module tail. */
    uint32_t *got_off = NULL;
    if (format->got_hi_type && nrela_text) {
        uint32_t ngot = 0;
        uint32_t got_hi_type = format->got_hi_type;
        for (uint32_t i = 0; i < nrela_text; i++)
            if (ELF_R_TYPE(rela_text[i].r_info) == got_hi_type)
                ngot++;
        if (ngot) {
            total_size = (total_size + 7) & ~7U;
            if (total_size + ngot * 8U > DRV_MOD_MAX_SIZE) {
                if (veneer_off)
                    kfree(veneer_off);
                drvmod_free_pages(buf_pfn, DRV_MOD_BUF_ORDER);
                return -ENOEXEC;
            }
            got_off = kmalloc(nrela_text * sizeof(uint32_t));
            if (!got_off) {
                if (veneer_off)
                    kfree(veneer_off);
                drvmod_free_pages(buf_pfn, DRV_MOD_BUF_ORDER);
                return -ENOMEM;
            }
            memset(got_off, 0, nrela_text * sizeof(uint32_t));
            uint32_t goff = total_size;
            for (uint32_t i = 0; i < nrela_text; i++) {
                if (ELF_R_TYPE(rela_text[i].r_info) == got_hi_type) {
                    got_off[i] = goff;
                    goff += 8;
                }
            }
            /* Pair every %got_pc_lo12 with the %got_pc_hi20 four bytes
             * before it (the pcalau12i + ld.d sequence).  Look the HI20
             * up by relocation offset: the list order is not guaranteed
             * to match address order. */
            if (format->got_lo_type != 0) {
                for (uint32_t i = 0; i < nrela_text; i++) {
                    if (ELF_R_TYPE(rela_text[i].r_info) != format->got_lo_type)
                        continue;
                    for (uint32_t j = 0; j < nrela_text; j++) {
                        if (ELF_R_TYPE(rela_text[j].r_info) == format->got_hi_type &&
                            rela_text[j].r_offset == rela_text[i].r_offset - 4) {
                            got_off[i] = got_off[j];
                            break;
                        }
                    }
                }
            }
            total_size += ngot * 8U;
        }
    }
    total_size = (total_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (total_size == 0 || total_size > DRV_MOD_MAX_SIZE) {
        kerr("[DRVMOD] %s: bad total_size %u\n", name, total_size);
        if (veneer_off)
            kfree(veneer_off);
        if (got_off)
            kfree(got_off);
        drvmod_free_pages(buf_pfn, DRV_MOD_BUF_ORDER);
        return -ENOEXEC;
    }
    /* Shadow buffer in load layout.  Allocated from the frame pool (not
     * kmalloc): kmalloc slabs draw from the same buddy list the module
     * pages are taken from, so a kmalloc shadow can silently overlap the
     * pfa_alloc'd module and freeing it later would release the module's
     * own pages to the allocator. */
    uint32_t npages = total_size / PAGE_SIZE;
    uint32_t alloc_order = 0;
    while ((1U << alloc_order) < npages)
        alloc_order++;
    pfn_t shadow_pfn = pfa_alloc((int)alloc_order);
    if (shadow_pfn == PFN_NONE) {
        printf("[DRVMOD] %s: shadow pfa_alloc %u failed\n", name,
               total_size);
        if (veneer_off)
            kfree(veneer_off);
        if (got_off)
            kfree(got_off);
        drvmod_free_pages(buf_pfn, DRV_MOD_BUF_ORDER);
        return -ENOMEM;
    }
    uint8_t *shadow = (uint8_t *)(PAGE_OFFSET +
                                  pfn_to_phys(shadow_pfn));
    memset(shadow, 0, total_size);
    if (veneer_off) {
        for (uint32_t i = 0; i < nrela_text; i++) {
            if (!veneer_off[i])
                continue;
            for (uint32_t word = 0; word < format->veneer_word_count; word++)
                *(uint32_t *)(shadow + veneer_off[i] + word * 4) =
                    format->veneer_words[word];
        }
    }
    for (uint32_t i = 0; i < nshdrs; i++) {
        elf_shdr_t *sh = &shdrs[i];
        if (sh->sh_type == SHT_PROGBITS) {
            uint32_t off, sec_size;
            if (drvmod_sec_lookup(secmap, nmap, i, &off, &sec_size) < 0)
                continue;
            if (!drvmod_range_ok32(off, sec_size, total_size)) {
                if (veneer_off)
                    kfree(veneer_off);
                if (got_off)
                    kfree(got_off);
                drvmod_free_pages(shadow_pfn, alloc_order);
                drvmod_free_pages(buf_pfn, DRV_MOD_BUF_ORDER);
                return -ENOEXEC;
            }
            memcpy(shadow + off, buf + sh->sh_offset, (size_t)sh->sh_size);
        }
    }

    /* Allocate physical pages first: relocations must be computed against
     * the FINAL load address (direct-map window: PAGE_OFFSET + PA), since
     * PC-relative displacements are relative to the runtime PC. */
    pfn_t alloc_pfn = PFN_NONE;
    uintptr_t load_base = 0;
    int reserved = arch_drvmod_alloc_reserved(alloc_order, &load_base);
    if (reserved < 0) {
        if (veneer_off)
            kfree(veneer_off);
        if (got_off)
            kfree(got_off);
        drvmod_free_pages(shadow_pfn, alloc_order);
        drvmod_free_pages(buf_pfn, DRV_MOD_BUF_ORDER);
        return reserved;
    }
    if (!reserved) {
        alloc_pfn = pfa_alloc((int)alloc_order);
        if (alloc_pfn == PFN_NONE) {
            if (veneer_off)
                kfree(veneer_off);
            if (got_off)
                kfree(got_off);
            drvmod_free_pages(shadow_pfn, alloc_order);
            drvmod_free_pages(buf_pfn, DRV_MOD_BUF_ORDER);
            return -ENOMEM;
        }
        load_base = PAGE_OFFSET + pfn_to_phys(alloc_pfn);
    }

    /* RISC-V only: pre-scan PCREL_HI20 targets (LO12 references the HI20
     * instruction).  Other machines are self-contained per relocation. */
    uint32_t nslots = 0;
    uintptr_t *hi_targets = NULL;
    if (format->pcrel_pairs) {
        nslots = total_size / 4 + 1;
        hi_targets = kmalloc(nslots * sizeof(uintptr_t));
        if (!hi_targets) {
            if (veneer_off)
                kfree(veneer_off);
            if (got_off)
                kfree(got_off);
            drvmod_free_image(load_base, alloc_pfn, alloc_order);
            drvmod_free_pages(shadow_pfn, alloc_order);
            drvmod_free_pages(buf_pfn, DRV_MOD_BUF_ORDER);
            return -ENOMEM;
        }
        memset(hi_targets, 0, nslots * sizeof(uintptr_t));
        for (uint32_t i = 0; i < nrela_sets; i++) {
            uint32_t off, size;
            if (drvmod_sec_lookup(secmap, nmap, rela_sets[i].target_sec,
                                  &off, &size) < 0)
                continue;
            arch_module_prescan_pcrel(rela_sets[i].rela, rela_sets[i].nrela,
                                 syms, nsyms, strtab, strtab_size,
                                 load_base, secmap, nmap, off, size,
                                 hi_targets, nslots,
                                 rela_sets[i].rela == rela_text ? got_off : NULL);
        }
    }

    /* Apply relocations on the shadow, one pass per RELA section. */
    for (uint32_t i = 0; i < nrela_sets; i++) {
        uint32_t off, size;
        if (drvmod_sec_lookup(secmap, nmap, rela_sets[i].target_sec,
                              &off, &size) < 0)
            continue;
        if (arch_module_apply_reloc(machine, shadow, load_base, secmap, nmap,
                               strtab, strtab_size, syms, nsyms,
                               rela_sets[i].rela, rela_sets[i].nrela,
                               off, size, hi_targets, nslots,
                               rela_sets[i].rela == rela_text ? veneer_off : NULL,
                               rela_sets[i].rela == rela_text ? got_off : NULL) ==
            0xFFFFFFFFU) {
            if (hi_targets)
                kfree(hi_targets);
            if (veneer_off)
                kfree(veneer_off);
            if (got_off)
                kfree(got_off);
            drvmod_free_image(load_base, alloc_pfn, alloc_order);
            drvmod_free_pages(shadow_pfn, alloc_order);
            drvmod_free_pages(buf_pfn, DRV_MOD_BUF_ORDER);
            return -EINVAL;
        }
    }
    /* Resolve DriverEntry before installing pages: it must be a global
     * symbol inside the placed .text section. */
    uintptr_t entry = 0;
    for (uint32_t i = 0; i < nsyms; i++) {
        elf_sym_t *sym = &syms[i];
        if (sym->st_shndx == 0)
            continue;
        const char *nm = drvmod_sym_name(sym, strtab, strtab_size);
        if (nm && strcmp(nm, "DriverEntry") == 0) {
            uint32_t soff, sec_size;
            if (drvmod_sec_lookup(secmap, nmap, sym->st_shndx,
                                  &soff, &sec_size) < 0 ||
                sec_size == 0 /* must be .text */ ||
                sym->st_value >= sec_size) {
                kerr("[DRVMOD] %s: DriverEntry not in .text\n", name);
                entry = 0;
                break;
            }
            entry = load_base + soff + sym->st_value;
            break;
        }
    }
    if (!entry) {
        if (hi_targets)
            kfree(hi_targets);
        if (veneer_off)
            kfree(veneer_off);
        if (got_off)
            kfree(got_off);
        drvmod_free_pages(shadow_pfn, alloc_order);
        drvmod_free_pages(buf_pfn, DRV_MOD_BUF_ORDER);
        drvmod_free_image(load_base, alloc_pfn, alloc_order);
        return -ENOENT;
    }

    if (hi_targets)
        kfree(hi_targets);
    if (veneer_off)
        kfree(veneer_off);
    if (got_off)
        kfree(got_off);
    drvmod_free_pages(buf_pfn, DRV_MOD_BUF_ORDER);

    memcpy((void *)load_base, shadow, total_size);
    arch_flush_icache_range((void *)load_base, total_size);
    drvmod_free_pages(shadow_pfn, alloc_order);

    /* Kernel W^X: the direct map is non-executable, so the module's text
     * region must be marked read-only/executable (and the data region
     * explicitly non-executable) before DriverEntry can run.  The text
     * region holds .text plus loader veneers and is page-aligned apart
     * from data by the layout above.
     *
     * Under NOMMU the arena block is already AP=00 -- privileged and
     * executable from the boot map -- so there is nothing to change and no
     * pfn to translate. */
    if (alloc_pfn != PFN_NONE &&
        arch_kwx_module_protect(pfn_to_phys(alloc_pfn), text_region_size,
                                total_size) < 0) {
        kerr("[DRVMOD] %s: cannot mark module text executable\n", name);
        drvmod_free_image(load_base, alloc_pfn, alloc_order);
        return -ENOMEM;
    }

    drv_module_t *m = &drv_modules[slot];
    memset(m, 0, sizeof(*m));
    m->used = 1;
    m->id = slot;
    strncpy(m->name, name, DRV_MOD_MAX_NAME - 1);
    m->base = load_base;
    m->entry = entry;
    m->total_size = total_size;
    m->alloc_pfn = alloc_pfn;
    m->alloc_order = alloc_order;

    printf("[DRVMOD] loaded '%s' id=%d base=0x%lx size=%u entry=0x%lx\n",
           name, slot, (unsigned long)load_base, total_size,
           (unsigned long)entry);
    return slot;
}

int drvmod_unload(int id)
{
    if (id < 0 || id >= DRV_MOD_MAX_MODULES || !drv_modules[id].used)
        return -ENOENT;
    drv_module_t *m = &drv_modules[id];
    if (m->pinned)
        return -EBUSY;
    kdebug("[DRVMOD] unload '%s'\n", m->name);
    /* Restore the module pages to the plain RW+NX direct-map state before
     * returning them to the allocator, so the next owner never inherits an
     * executable (or read-only) mapping. */
    if (m->alloc_pfn == PFN_NONE) {
        arch_drvmod_free_reserved(m->base, m->alloc_order);
    } else {
        arch_kwx_module_unprotect(pfn_to_phys(m->alloc_pfn), m->total_size);
        drvmod_free_pages(m->alloc_pfn, m->alloc_order);
    }
    memset(m, 0, sizeof(*m));
    return 0;
}

int drvmod_unload_by_name(const char *name)
{
    if (!name)
        return -EINVAL;
    for (int i = 0; i < DRV_MOD_MAX_MODULES; i++) {
        drv_module_t *m = &drv_modules[i];
        if (!m->used)
            continue;
        if (strcmp(m->name, name) == 0)
            return drvmod_unload(i);
    }
    return -ENOENT;
}

/* Format loaded modules for /proc/modules (Linux format). */
int drvmod_list(char *buf, size_t sz)
{
    if (!buf)
        return -EINVAL;
    size_t off = 0;
    for (int i = 0; i < DRV_MOD_MAX_MODULES; i++) {
        drv_module_t *m = &drv_modules[i];
        if (!m->used)
            continue;
        int n = snprintf(buf + off, sz > off ? sz - off : 0,
                         "%s %u 0 %d 0 Live 0x%lx\n",
                         m->name, (unsigned)m->total_size,
                         m->pinned ? 0 : -1,
                         (unsigned long)m->base);
        if (n < 0)
            break;
        off += (size_t)n;
        if (off >= sz)
            break;
    }
    return (int)off;
}

/* Format registered drivers for /proc/drivers. */
int drvmod_drivers(char *buf, size_t sz)
{
    extern int driver_core_list_drivers(char *buf, size_t sz);
    return driver_core_list_drivers(buf, sz);
}

/* Run DriverEntry for every loaded module.  Each module registers its
 * unified driver_t with the driver core; binding happens through the core's
 * normal match/probe path.  Modules that registered a driver are pinned.
 * Pinned modules are skipped on later scans (e.g. the runtime DriverStore
 * scan must not re-run an early module's DriverEntry). */
void drvmod_init_all(void)
{
    for (int i = 0; i < DRV_MOD_MAX_MODULES; i++) {
        drv_module_t *m = &drv_modules[i];
        if (!m->used)
            continue;
        if (m->pinned)
            continue;
        if (m->entry) {
            uintptr_t (*DriverEntry)(void) = (uintptr_t (*)(void))m->entry;
            arch_flush_icache_range((void *)m->base, m->total_size);
            arch_mb();
            uintptr_t r = DriverEntry();
            if (r == 0) {
                m->pinned = 1;
                kdebug("[DRVMOD] '%s' DriverEntry ok\n", m->name);
            } else {
                printf("[DRVMOD] '%s' DriverEntry failed\n", m->name);
            }
        }
    }
}

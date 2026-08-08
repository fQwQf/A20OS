/*
 * A20OS kernel driver modules — ELF loader (multi-architecture).
 *
 * Loads a driver module (ELF relocatable object, ET_REL) into the kernel
 * direct map and resolves its external symbols against the driver
 * framework export table ONLY (symbol whitelist).  Supported machines and
 * the relocation sets emitted by their DRVMOD_CFLAGS (see Makefile):
 *
 *   riscv64     R_RISCV_*      (medany, -fno-pic; external calls use
 *                               loader veneers when outside +/-2 GiB)
 *   x86_64      R_X86_64_*     (large model: ABS64 for externals, no
 *                               PC32/PLT32 range limit)
 *   aarch64     R_AARCH64_*    (BL range is ±128 MiB; kernel text and the
 *                               module window share the direct map)
 *   loongarch64 R_LARCH_*      (B26 ±128 MiB / PCALA32; complex 64-bit
 *                               PCALA chains only appear in .eh_frame,
 *                               which the loader drops)
 *
 * Every pointer derived from the module file (section table, symbol table,
 * string tables, relocation entries) is bounds-checked against the file
 * buffer before use; a malformed module is rejected instead of corrupting
 * the kernel heap.
 */

#include "drvmod/drvmod.h"

#include "core/klog.h"
#include "core/string.h"
#include "core/panic.h"
#include "fs/vfs.h"
#include "mm/frame.h"
#include "mm/mm.h"
#include "mm/slab.h"

#define DRV_MOD_MAX_MODULES  32

#define ELFCLASS64 2
#define ELFDATA2LSB 1
#define EM_RISCV    243
#define EM_X86_64   62
#define EM_AARCH64  183
#define EM_LOONGARCH 258
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

/* ---- ELF structures (module files are always ELF64) ---- */

typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} elf_ehdr_t;

typedef struct {
    uint32_t sh_name, sh_type, sh_flags;
    uint64_t sh_addr, sh_offset, sh_size;
    uint32_t sh_link, sh_info;
    uint64_t sh_addralign, sh_entsize;
} elf_shdr_t;

typedef struct {
    uint32_t st_name;
    uint8_t  st_info, st_other;
    uint16_t st_shndx;
    uint64_t st_value, st_size;
} elf_sym_t;

typedef struct {
    uint64_t r_offset;
    uint64_t r_info;
    int64_t  r_addend;
} elf_rela_t;

#define ELF_R_TYPE(info)   ((uint32_t)(info) & 0xffffffffU)
#define ELF_R_SYM(info)    ((uint32_t)((info) >> 32))

#define SHT_PROGBITS 1
#define SHT_SYMTAB 2
#define SHT_NOBITS 8
#define SHT_RELA   4

/* ---- relocation type numbers ---- */

/* RISC-V */
#define R_RISCV_32          1
#define R_RISCV_64          2
#define R_RISCV_RELATIVE    3
#define R_RISCV_HI20        26
#define R_RISCV_LO12_I      27
#define R_RISCV_LO12_S      28
#define R_RISCV_BRANCH        16
#define R_RISCV_JAL           17
#define R_RISCV_CALL          18
#define R_RISCV_CALL_PLT      19
#define R_RISCV_PCREL_HI20    23
#define R_RISCV_PCREL_LO12_I  24
#define R_RISCV_PCREL_LO12_S  25

/* x86-64 */
#define R_X86_64_64        1
#define R_X86_64_PC32      2
#define R_X86_64_PLT32     4
#define R_X86_64_RELATIVE  8
#define R_X86_64_32        10
#define R_X86_64_32S       11
#define R_X86_64_PC64      24

/* AArch64 */
#define R_AARCH64_ABS32         258
#define R_AARCH64_ABS64         257
#define R_AARCH64_PREL64        260
#define R_AARCH64_PREL32        261
#define R_AARCH64_JUMP26        282
#define R_AARCH64_CALL26        283
#define R_AARCH64_ADR_PREL_LO21     274
#define R_AARCH64_ADR_PREL_PG_HI21  275
#define R_AARCH64_ADD_ABS_LO12_NC   277
#define R_AARCH64_LDST_ABS_LO12_NC  278
#define R_AARCH64_RELATIVE      1027

/* LoongArch64 (numbers per the LoongArch ELF psABI) */
#define R_LARCH_32          1
#define R_LARCH_64          2
#define R_LARCH_RELATIVE    3
#define R_LARCH_ADD8        36
#define R_LARCH_SUB8        41
#define R_LARCH_ADD32       39
#define R_LARCH_SUB32       44
#define R_LARCH_B16         64
#define R_LARCH_B21         65
#define R_LARCH_B26         66
#define R_LARCH_PCALA_HI20  71
#define R_LARCH_PCALA_LO12  72
#define R_LARCH_GOT_PC_HI20 75
#define R_LARCH_GOT_PC_LO12 76
#define R_LARCH_32_PCREL    99
#define R_LARCH_RELAX       100
#define R_LARCH_64_PCREL    109
#define R_LARCH_CALL36      110

/* Framework export table (framework.c). */
extern const struct drv_export {
    const char *name;
    void *addr;
} drv_export_table[];
extern const unsigned drv_export_count;

static void *drvmod_resolve_symbol(const char *name)
{
    for (unsigned i = 0; i < drv_export_count; i++) {
        if (strcmp(drv_export_table[i].name, name) == 0)
            return drv_export_table[i].addr;
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

typedef struct drvmod_secmap {
    uint32_t idx;
    uint32_t load_off;
    uint32_t size;
} drvmod_secmap_t;

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

/* Resolve a relocation symbol to its final address.  Returns 0 on success;
 * on failure *err is set and the result must not be used. */
static int drvmod_sym_addr(const elf_sym_t *sym, const char *strtab,
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

/* Bytes touched by a relocation in the target section (0 = marker/no-op). */
static uint32_t drvmod_reloc_width(uint32_t machine, uint32_t type)
{
    switch (machine) {
    case EM_RISCV:
        switch (type) {
        case R_RISCV_RELATIVE:
        case R_RISCV_64:
            return 8;
        case R_RISCV_CALL:
        case R_RISCV_CALL_PLT:
            return 8;       /* auipc + jalr pair */
        default:
            return 4;
        }
    case EM_X86_64:
        switch (type) {
        case R_X86_64_64:
        case R_X86_64_RELATIVE:
        case R_X86_64_PC64:
            return 8;
        default:
            return 4;
        }
    case EM_AARCH64:
        switch (type) {
        case R_AARCH64_ABS64:
        case R_AARCH64_PREL64:
        case R_AARCH64_RELATIVE:
            return 8;
        default:
            return 4;
        }
    case EM_LOONGARCH:
        switch (type) {
        case R_LARCH_64:
        case R_LARCH_RELATIVE:
            return 8;
        case R_LARCH_RELAX:
            return 0;
        default:
            return 4;
        }
    }
    return 0;
}

/* RISC-V pre-scan: PCREL_HI20 relocations record the target of each HI20
 * instruction, so the paired PCREL_LO12 can compute its own displacement
 * (RISC-V psABI: LO12 references the symbol whose value is the HI20
 * instruction offset).  Only RISC-V needs this pairing. */
static void drvmod_prescan_pcrel(elf_rela_t *rela, uint32_t nrela,
                                  elf_sym_t *syms, uint32_t nsyms,
                                  const char *strtab, uint32_t strtab_size,
                                  uintptr_t load_base,
                                  drvmod_secmap_t *map, uint32_t nmap,
                                  uint32_t sec_load_off, uint32_t sec_size,
                                  uintptr_t *hi_targets, uint32_t ntargets)
{
    for (uint32_t i = 0; i < nrela; i++) {
        elf_rela_t *r = &rela[i];
        uint32_t type = ELF_R_TYPE(r->r_info);
        if (type != R_RISCV_PCREL_HI20)
            continue;
        if (!drvmod_range_ok32((uint32_t)r->r_offset, 4, sec_size))
            continue;
        uint32_t symidx = ELF_R_SYM(r->r_info);
        if (symidx >= nsyms)
            continue;
        uintptr_t S;
        if (drvmod_sym_addr(&syms[symidx], strtab, strtab_size, map, nmap,
                            load_base, &S) < 0)
            continue;
        uint32_t load_off = sec_load_off + (uint32_t)r->r_offset;
        if (load_off < ntargets * 8)
            hi_targets[load_off / 8] = S;
    }
}

/* AArch64 ADRP: encode imm21 (page delta) into immlo/immhi fields. */
static void drvmod_a64_adrp(uint32_t *insn, int64_t delta)
{
    uint32_t imm = (uint32_t)((delta >> 12) & 0x1FFFFF);
    *insn = (*insn & 0x9F00001F) | ((imm & 0x3) << 29) | ((imm & 0x1FFFFC) << 5);
}

/* AArch64 ADR: encode imm21 (byte delta). */
static void drvmod_a64_adr(uint32_t *insn, int64_t delta)
{
    uint32_t imm = (uint32_t)(delta & 0x1FFFFF);
    *insn = (*insn & 0x9F00001F) | ((imm & 0x3) << 29) | ((imm & 0x1FFFFC) << 5);
}

static uint32_t drvmod_apply_reloc(uint32_t machine, uint8_t *shadow,
                                   uintptr_t load_base,
                                   drvmod_secmap_t *map, uint32_t nmap,
                                   const char *strtab, uint32_t strtab_size,
                                   elf_sym_t *syms, uint32_t nsyms,
                                   elf_rela_t *rela, uint32_t nrela,
                                   uint32_t sec_load_off, uint32_t sec_size,
                                   const uintptr_t *hi_targets,
                                   uint32_t ntargets,
                                   const uint32_t *veneer_off,
                                   const uint32_t *got_off)
{
    for (uint32_t i = 0; i < nrela; i++) {
        elf_rela_t *r = &rela[i];
        uint32_t type = ELF_R_TYPE(r->r_info);
        uint32_t symidx = ELF_R_SYM(r->r_info);

        uint32_t width = drvmod_reloc_width(machine, type);
        if (width == 0)
            continue;       /* marker relocation (e.g. R_LARCH_RELAX) */
        if (!drvmod_range_ok32((uint32_t)r->r_offset, width, sec_size)) {
            printf("[DRVMOD] reloc offset 0x%lx escapes section\n",
                 (unsigned long)r->r_offset);
            return 0xFFFFFFFFU;
        }

        uintptr_t P = load_base + sec_load_off + r->r_offset;
        uintptr_t S = 0;

        if (symidx != 0) {
            if (symidx >= nsyms) {
                printf("[DRVMOD] bad symbol index %u\n", symidx);
                return 0xFFFFFFFFU;
            }
            if (drvmod_sym_addr(&syms[symidx], strtab, strtab_size,
                                map, nmap, load_base, &S) < 0)
                return 0xFFFFFFFFU;
        }

        switch (machine) {
        case EM_RISCV: {
            switch (type) {
            case R_RISCV_RELATIVE:
                *(uint64_t *)(shadow + sec_load_off + r->r_offset) =
                    (uint64_t)(P + r->r_addend);
                break;
            case R_RISCV_64:
                *(uint64_t *)(shadow + sec_load_off + r->r_offset) =
                    S + r->r_addend;
                break;
            case R_RISCV_32:
                *(uint32_t *)(shadow + sec_load_off + r->r_offset) =
                    (uint32_t)(S + r->r_addend);
                break;
            case R_RISCV_BRANCH: {
                int32_t off = (int32_t)(S - P);
                uint32_t insn = *(uint32_t *)(shadow + sec_load_off + r->r_offset);
                uint32_t lo = (uint32_t)off & 0x1FFF;
                insn = (insn & 0xFE0FF07F) | ((lo & 0x1F) << 7) |
                       (((lo >> 5) & 0x3F) << 25) | (((lo >> 11) & 0x1) << 31);
                *(uint32_t *)(shadow + sec_load_off + r->r_offset) = insn;
                break;
            }
            case R_RISCV_JAL: {
                int32_t off = (int32_t)(S - P);
                uint32_t insn = *(uint32_t *)(shadow + sec_load_off + r->r_offset);
                uint32_t lo = (uint32_t)off & 0x1FFFFF;
                insn = (insn & 0xFFF) | ((lo & 0xFF000) << 1) |
                       ((lo & 0x800) << 9) | ((lo & 0x7FE) << 20) |
                       ((lo & 0x100000) << 11);
                *(uint32_t *)(shadow + sec_load_off + r->r_offset) = insn;
                break;
            }
            case R_RISCV_CALL_PLT:
            case R_RISCV_CALL:
                /* auipc + jalr pair: S - P split into hi20/lo12. */
                {
                    int64_t off64 = (int64_t)(S + r->r_addend - P);
                    if (off64 < -0x80000000LL || off64 > 0x7fffffffLL) {
                        /* medany CALL only covers a signed 32-bit PC-relative
                         * displacement.  With an 8 GiB direct map the buddy
                         * allocator may place a module farther from kernel
                         * text, so tail-call through a module-local veneer:
                         *
                         *   auipc t0, 0; ld t0, 16(t0); jalr zero, 0(t0)
                         *   nop; .quad S
                         */
                        if (!veneer_off || veneer_off[i] == 0) {
                            printf("[DRVMOD] riscv64 call out of range (0x%lx)\n",
                                   (unsigned long)off64);
                            return 0xFFFFFFFFU;
                        }
                        uintptr_t veneer = load_base + veneer_off[i];
                        off64 = (int64_t)(veneer - P);
                        if (off64 < -0x80000000LL || off64 > 0x7fffffffLL) {
                            printf("[DRVMOD] riscv64 veneer out of range (0x%lx)\n",
                                   (unsigned long)off64);
                            return 0xFFFFFFFFU;
                        }
                        *(uint64_t *)(shadow + veneer_off[i] + 16) =
                            S + r->r_addend;
                    }
                    int32_t off = (int32_t)off64;
                    uint32_t hi =
                        (uint32_t)((off64 + 0x800LL) >> 12) & 0xFFFFF;
                    uint32_t lo = (uint32_t)off & 0xFFF;
                    uint32_t *auipc = (uint32_t *)(shadow + sec_load_off + r->r_offset);
                    *auipc = (*auipc & 0xFFF) | ((hi & 0xFFFFF) << 12);
                    uint32_t *jalr = (uint32_t *)(shadow + sec_load_off + r->r_offset + 4);
                    *jalr = (*jalr & 0xFFFFF) | ((lo & 0xFFF) << 20);
                }
                break;
            case R_RISCV_HI20: {
                uint32_t insn = *(uint32_t *)(shadow + sec_load_off + r->r_offset);
                int32_t addend = (int32_t)((insn >> 12) & 0xFFFFF);
                int32_t off = (int32_t)(S - (uintptr_t)addend);
                uint32_t hi = (uint32_t)((off + 0x800) >> 12) & 0xFFFFF;
                *(uint32_t *)(shadow + sec_load_off + r->r_offset) =
                    (insn & 0xFFF) | (hi << 12);
                break;
            }
            case R_RISCV_PCREL_HI20: {
                uint32_t insn = *(uint32_t *)(shadow + sec_load_off + r->r_offset);
                int32_t addend = (int32_t)((insn >> 12) & 0xFFFFF);
                int32_t off = (int32_t)(S - P + addend);
                uint32_t hi = (uint32_t)((off + 0x800) >> 12) & 0xFFFFF;
                *(uint32_t *)(shadow + sec_load_off + r->r_offset) =
                    (insn & 0xFFF) | (hi << 12);
                break;
            }
            case R_RISCV_PCREL_LO12_I:
            case R_RISCV_PCREL_LO12_S: {
                /* The target is recorded by the prescan for the HI20
                 * instruction this LO12 references (symbol st_value). */
                uintptr_t hi_off = (uintptr_t)S - load_base;
                uintptr_t target = 0;
                if (hi_targets && (hi_off / 8) < ntargets)
                    target = hi_targets[hi_off / 8];
                uintptr_t p_hi = load_base + hi_off;
                int32_t off = (int32_t)(target - p_hi);
                if (type == R_RISCV_PCREL_LO12_I) {
                    uint32_t insn = *(uint32_t *)(shadow + sec_load_off + r->r_offset);
                    *(uint32_t *)(shadow + sec_load_off + r->r_offset) =
                        (insn & 0xFFFFF) | (((uint32_t)off & 0xFFF) << 20);
                } else {
                    uint32_t insn = *(uint32_t *)(shadow + sec_load_off + r->r_offset);
                    uint32_t lo = (uint32_t)off & 0xFFF;
                    *(uint32_t *)(shadow + sec_load_off + r->r_offset) =
                        (insn & 0xFFFFF000) | ((lo & 0x1F) << 7) |
                        (((lo >> 5) & 0x7F) << 25);
                }
                break;
            }
            default:
                printf("[DRVMOD] unsupported riscv reloc %u\n", type);
                return 0xFFFFFFFFU;
            }
            break;
        }
        case EM_X86_64: {
            switch (type) {
            case R_X86_64_64:
            case R_X86_64_RELATIVE:
                *(uint64_t *)(shadow + sec_load_off + r->r_offset) =
                    (uint64_t)((type == R_X86_64_RELATIVE ? P : S) +
                               r->r_addend);
                break;
            case R_X86_64_PC64:
                *(uint64_t *)(shadow + sec_load_off + r->r_offset) =
                    (uint64_t)(S + r->r_addend - P);
                break;
            case R_X86_64_PC32:
            case R_X86_64_PLT32: {
                int64_t v = (int64_t)(S + r->r_addend - P);
                if (v < -0x80000000LL || v > 0x7FFFFFFFLL) {
                    printf("[DRVMOD] x86 PC32 overflow (0x%lx)\n",
                         (unsigned long)v);
                    return 0xFFFFFFFFU;
                }
                *(uint32_t *)(shadow + sec_load_off + r->r_offset) =
                    (uint32_t)v;
                break;
            }
            case R_X86_64_32: {
                uint64_t v = S + r->r_addend;
                if (v > 0xFFFFFFFFULL) {
                    printf("[DRVMOD] x86 32 overflow (0x%lx)\n",
                         (unsigned long)v);
                    return 0xFFFFFFFFU;
                }
                *(uint32_t *)(shadow + sec_load_off + r->r_offset) =
                    (uint32_t)v;
                break;
            }
            case R_X86_64_32S: {
                int64_t v = (int64_t)(S + r->r_addend);
                if (v < -0x80000000LL || v > 0x7FFFFFFFLL) {
                    printf("[DRVMOD] x86 32S overflow (0x%lx)\n",
                         (unsigned long)v);
                    return 0xFFFFFFFFU;
                }
                *(uint32_t *)(shadow + sec_load_off + r->r_offset) =
                    (uint32_t)v;
                break;
            }
            default:
                printf("[DRVMOD] unsupported x86_64 reloc %u\n", type);
                return 0xFFFFFFFFU;
            }
            break;
        }
        case EM_AARCH64: {
            switch (type) {
            case R_AARCH64_ABS64:
            case R_AARCH64_RELATIVE:
                *(uint64_t *)(shadow + sec_load_off + r->r_offset) =
                    (uint64_t)((type == R_AARCH64_RELATIVE ? P : S) +
                               r->r_addend);
                break;
            case R_AARCH64_ABS32:
                *(uint32_t *)(shadow + sec_load_off + r->r_offset) =
                    (uint32_t)(S + r->r_addend);
                break;
            case R_AARCH64_PREL64:
                *(uint64_t *)(shadow + sec_load_off + r->r_offset) =
                    (uint64_t)(S + r->r_addend - P);
                break;
            case R_AARCH64_PREL32:
                *(uint32_t *)(shadow + sec_load_off + r->r_offset) =
                    (uint32_t)(S + r->r_addend - P);
                break;
            case R_AARCH64_CALL26:
            case R_AARCH64_JUMP26: {
                int64_t off = (int64_t)(S + r->r_addend - P);
                uint32_t insn = *(uint32_t *)(shadow + sec_load_off + r->r_offset);
                if (off >= -0x8000000LL && off <= 0x7FFFFFFLL) {
                    /* Direct branch. */
                    *(uint32_t *)(shadow + sec_load_off + r->r_offset) =
                        (insn & 0xFC000000) | (((uint32_t)off >> 2) & 0x3FFFFFF);
                    break;
                }
                /* Out of ±128 MiB: route through a loader-generated veneer
                 * (ldr x16, [pc, #8]; br x16; .quad S) at the module tail. */
                if (!veneer_off || veneer_off[i] == 0) {
                    printf("[DRVMOD] aarch64 branch out of range (0x%lx)\n",
                           (unsigned long)off);
                    return 0xFFFFFFFFU;
                }
                uintptr_t veneer = load_base + veneer_off[i];
                int64_t voff = (int64_t)(veneer - P);
                *(uint32_t *)(shadow + sec_load_off + r->r_offset) =
                    (insn & 0xFC000000) | (((uint32_t)voff >> 2) & 0x3FFFFFF);
                *(uint64_t *)(shadow + veneer_off[i] + 8) = S + r->r_addend;
                break;
            }
            case R_AARCH64_ADR_PREL_PG_HI21: {
                int64_t delta = (int64_t)(S + r->r_addend) - (int64_t)(P & ~0xFFFLL);
                if (delta > 0x7FFFFFFFLL || delta < -0x80000000LL) {
                    printf("[DRVMOD] aarch64 ADRP out of range (0x%lx)\n",
                         (unsigned long)delta);
                    return 0xFFFFFFFFU;
                }
                drvmod_a64_adrp((uint32_t *)(shadow + sec_load_off + r->r_offset),
                                delta);
                break;
            }
            case R_AARCH64_ADR_PREL_LO21: {
                int64_t delta = (int64_t)(S + r->r_addend - P);
                if (delta > 0xFFFFFLL || delta < -0x100000LL) {
                    printf("[DRVMOD] aarch64 ADR out of range (0x%lx)\n",
                         (unsigned long)delta);
                    return 0xFFFFFFFFU;
                }
                drvmod_a64_adr((uint32_t *)(shadow + sec_load_off + r->r_offset),
                               delta);
                break;
            }
            case R_AARCH64_ADD_ABS_LO12_NC:
            case R_AARCH64_LDST_ABS_LO12_NC: {
                uint32_t insn = *(uint32_t *)(shadow + sec_load_off + r->r_offset);
                uint32_t imm = (uint32_t)(S + r->r_addend) & 0xFFF;
                *(uint32_t *)(shadow + sec_load_off + r->r_offset) =
                    (insn & 0xFFC003FF) | (imm << 10);
                break;
            }
            default:
                printf("[DRVMOD] unsupported aarch64 reloc %u\n", type);
                return 0xFFFFFFFFU;
            }
            break;
        }
        case EM_LOONGARCH: {
            switch (type) {
            case R_LARCH_64:
            case R_LARCH_RELATIVE:
                *(uint64_t *)(shadow + sec_load_off + r->r_offset) =
                    (uint64_t)((type == R_LARCH_RELATIVE ? P : S) +
                               r->r_addend);
                break;
            case R_LARCH_32:
                *(uint32_t *)(shadow + sec_load_off + r->r_offset) =
                    (uint32_t)(S + r->r_addend);
                break;
            case R_LARCH_32_PCREL:
                *(uint32_t *)(shadow + sec_load_off + r->r_offset) =
                    (uint32_t)(S + r->r_addend - P);
                break;
            case R_LARCH_64_PCREL:
                *(uint64_t *)(shadow + sec_load_off + r->r_offset) =
                    (uint64_t)(S + r->r_addend - P);
                break;
            case R_LARCH_B16: {
                int64_t off = (int64_t)(S + r->r_addend - P);
                if (off < -0x20000LL || off > 0x1FFFFLL) {
                    printf("[DRVMOD] loongarch B16 out of range (0x%lx)\n",
                         (unsigned long)off);
                    return 0xFFFFFFFFU;
                }
                /* beqz/bnez/bgez family and beq/bne (16-bit form):
                 * offs[15:0] at bits [25:10]; rd[4:0] must be preserved
                 * (beq/bne encode a second register there). */
                uint32_t insn = *(uint32_t *)(shadow + sec_load_off + r->r_offset);
                *(uint32_t *)(shadow + sec_load_off + r->r_offset) =
                    (insn & 0xFC0003FF) |
                    ((((uint32_t)off >> 2) & 0xFFFF) << 10);
                break;
            }
            case R_LARCH_B21: {
                int64_t off = (int64_t)(S + r->r_addend - P);
                if (off < -0x200000LL || off > 0x1FFFFFLL) {
                    printf("[DRVMOD] loongarch B21 out of range (0x%lx)\n",
                         (unsigned long)off);
                    return 0xFFFFFFFFU;
                }
                uint32_t imm = ((uint32_t)off >> 2) & 0x1FFFFF;
                uint32_t insn = *(uint32_t *)(shadow + sec_load_off + r->r_offset);
                *(uint32_t *)(shadow + sec_load_off + r->r_offset) =
                    (insn & 0xFC0003E0) | ((imm & 0xFFFF) << 10) |
                    ((imm >> 16) & 0x1F);
                break;
            }
            case R_LARCH_B26:
            case R_LARCH_CALL36: {
                int64_t off = (int64_t)(S + r->r_addend - P);
                if (type == R_LARCH_B26) {
                    if (off < -0x8000000LL || off > 0x7FFFFFFLL) {
                        printf("[DRVMOD] loongarch B26 out of range (0x%lx)\n",
                             (unsigned long)off);
                        return 0xFFFFFFFFU;
                    }
                    /* b/bl: imm26 split as [15:0]@[25:10], [20:16]@[9:5],
                     * [25:21]@[4:0] (units of 4). */
                    uint32_t imm = ((uint32_t)off >> 2) & 0x3FFFFFF;
                    uint32_t insn = *(uint32_t *)(shadow + sec_load_off + r->r_offset);
                    *(uint32_t *)(shadow + sec_load_off + r->r_offset) =
                        (insn & 0xFC000000) |
                        ((imm & 0xFFFF) << 10) |
                        (((imm >> 16) & 0x1F) << 5) |
                        ((imm >> 21) & 0x1F);
                } else {
                    /* call36: pcaddu18i (si20 @ [24:5], 2^18 units) +
                     * jirl (imm16 @ [25:10], units of 4). */
                    if (off > 0x1FFFFFFFFLL || off < -0x200000000LL) {
                        printf("[DRVMOD] loongarch CALL36 out of range (0x%lx)\n",
                             (unsigned long)off);
                        return 0xFFFFFFFFU;
                    }
                    uint32_t hi = (uint32_t)((off + 0x20000) >> 18) & 0xFFFFF;
                    uint32_t lo = ((uint32_t)off >> 2) & 0xFFFF;
                    uint32_t *pc = (uint32_t *)(shadow + sec_load_off + r->r_offset);
                    *pc = (*pc & 0xFE00001F) | (hi << 5);
                    uint32_t *jr = (uint32_t *)(shadow + sec_load_off + r->r_offset + 4);
                    *jr = (*jr & 0xFC0003FF) | (lo << 10);
                }
                break;
            }
            case R_LARCH_GOT_PC_HI20: {
                if (!got_off || !got_off[i]) {
                    printf("[DRVMOD] loongarch GOT without slot (0x%lx)\n",
                           (unsigned long)r->r_offset);
                    return 0xFFFFFFFFU;
                }
                uintptr_t got = load_base + got_off[i];
                *(uint64_t *)(shadow + got_off[i]) = S + r->r_addend;
                uint32_t hi = ((uint32_t)(got >> 12) -
                               (uint32_t)(P >> 12)) & 0xFFFFF;
                uint32_t insn = *(uint32_t *)(shadow + sec_load_off + r->r_offset);
                *(uint32_t *)(shadow + sec_load_off + r->r_offset) =
                    (insn & 0xFE00001F) | (hi << 5);
                break;
            }
            case R_LARCH_GOT_PC_LO12: {
                if (!got_off || !got_off[i]) {
                    printf("[DRVMOD] loongarch GOT without slot (0x%lx)\n",
                           (unsigned long)r->r_offset);
                    return 0xFFFFFFFFU;
                }
                uintptr_t got = load_base + got_off[i];
                /* ld.d/ld.w displacements are unscaled byte offsets; the
                 * immediate carries the full low 12 bits of the slot. */
                uint32_t imm = (uint32_t)(got & 0xFFF);
                uint32_t insn = *(uint32_t *)(shadow + sec_load_off + r->r_offset);
                *(uint32_t *)(shadow + sec_load_off + r->r_offset) =
                    (insn & 0xFFC003FF) | (imm << 10);
                break;
            }
            case R_LARCH_PCALA_HI20: {
                uint32_t insn = *(uint32_t *)(shadow + sec_load_off + r->r_offset);
                uint32_t imm;
                if ((insn >> 25) == 0x0D) {
                    /* pcalau12i: page-relative, and addi.d's imm12 is
                     * sign-extended, so low-12 >= 0x800 carries into hi. */
                    uint32_t lo = (uint32_t)(S + r->r_addend) & 0xFFF;
                    imm = ((uint32_t)((S + r->r_addend) >> 12) -
                           (uint32_t)(P >> 12) + (lo >= 0x800 ? 1U : 0U)) &
                          0xFFFFF;
                } else {
                    /* pcaddi: byte-relative, rounded. */
                    int64_t off = (int64_t)(S + r->r_addend - P);
                    if (off > 0x7FFFFFFFLL || off < -0x80000000LL) {
                        printf("[DRVMOD] loongarch PCALA out of range (0x%lx)\n",
                             (unsigned long)off);
                        return 0xFFFFFFFFU;
                    }
                    imm = (uint32_t)((off + 0x800) >> 12) & 0xFFFFF;
                }
                *(uint32_t *)(shadow + sec_load_off + r->r_offset) =
                    (insn & 0xFE00001F) | (imm << 5);
                break;
            }
            case R_LARCH_PCALA_LO12: {
                uint32_t insn = *(uint32_t *)(shadow + sec_load_off + r->r_offset);
                uint32_t prev = (r->r_offset >= 4)
                    ? *(uint32_t *)(shadow + sec_load_off + r->r_offset - 4)
                    : 0;
                uint32_t imm;
                if ((prev >> 25) == 0x0D) {
                    /* pcalau12i pair: lo = low 12 bits of the target as a
                     * sign-extended 12-bit immediate. */
                    uint32_t raw = (uint32_t)(S + r->r_addend) & 0xFFF;
                    imm = (raw >= 0x800 ? raw - 0x1000 : raw) & 0xFFF;
                } else {
                    /* pcaddi pair: lo = byte displacement from the pcaddi
                     * (4 bytes before this addi.d). */
                    imm = (uint32_t)(S + r->r_addend - (P - 4)) & 0xFFF;
                }
                *(uint32_t *)(shadow + sec_load_off + r->r_offset) =
                    (insn & 0xFFC003FF) | (imm << 10);
                break;
            }
            default:
                printf("[DRVMOD] unsupported loongarch reloc %u\n", type);
                return 0xFFFFFFFFU;
            }
            break;
        }
        default:
            printf("[DRVMOD] machine %u not supported by loader\n", machine);
            return 0xFFFFFFFFU;
        }
    }
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
    switch (machine) {
    case EM_RISCV:
    case EM_X86_64:
    case EM_AARCH64:
    case EM_LOONGARCH:
        break;
    default:
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

    if (!descriptor || descriptor->magic != A20_DRIVER_DESCRIPTOR_MAGIC ||
        descriptor->version != A20_DRIVER_DESCRIPTOR_VERSION ||
        descriptor->placement != A20_DRIVER_PLACEMENT_KERNEL_MODULE ||
        descriptor->type < A20_DRIVER_TYPE_RTC ||
        descriptor->type > A20_DRIVER_TYPE_USB || !descriptor->name[0]) {
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
    uint32_t rela_text_sec = 0;
    uint32_t rela_text_off = 0, rela_text_size = 0;
    drvmod_secmap_t secmap[DRV_MOD_MAX_PLACED];
    uint32_t nmap = 0;

    /* Layout: .text at 0; each data/bss section follows it. */
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
            total_size = text_size;
        } else if (strcmp(n, ".data") == 0 || strcmp(n, ".rodata") == 0 ||
                   strcmp(n, ".sdata") == 0 || strcmp(n, ".bss") == 0 ||
                   strcmp(n, ".sbss") == 0 || strcmp(n, ".ldata") == 0 ||
                   strcmp(n, ".lbss") == 0) {
            if (nmap >= DRV_MOD_MAX_PLACED) {
                kerr("[DRVMOD] %s: too many loadable sections\n", name);
                drvmod_free_pages(buf_pfn, DRV_MOD_BUF_ORDER);
                return -ENOEXEC;
            }
            uint32_t align = (uint32_t)sh->sh_addralign;
            if (align < 4)
                align = 4;
            total_size = (total_size + align - 1) & ~(align - 1);
            secmap[nmap].idx = i;
            secmap[nmap].load_off = total_size;
            secmap[nmap].size = (uint32_t)sh->sh_size;
            nmap++;
            total_size += (uint32_t)sh->sh_size;
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
            rela_text = rela_sets[i].rela;
            nrela_text = rela_sets[i].nrela;
            rela_text_sec = tidx;
            if (drvmod_sec_lookup(secmap, nmap, tidx, &rela_text_off,
                                  &rela_text_size) < 0)
                rela_text_off = rela_text_size = 0;
        }
    }

    /* AArch64 and RISC-V external calls may need a veneer at the module
     * tail.  Reserve one for every external call relocation before the
     * final load address is known; relocation only uses it when the direct
     * displacement is out of range. */
    uint32_t *veneer_off = NULL;
    uint32_t nveneers = 0;
    uint32_t veneer_size = machine == EM_RISCV ? 24U : 16U;
    uint32_t veneer_align = machine == EM_RISCV ? 8U : 16U;
    if ((machine == EM_AARCH64 || machine == EM_RISCV) && nrela_text) {
        for (uint32_t i = 0; i < nrela_text; i++) {
            uint32_t type = ELF_R_TYPE(rela_text[i].r_info);
            int is_call = machine == EM_AARCH64
                ? (type == R_AARCH64_CALL26 || type == R_AARCH64_JUMP26)
                : (type == R_RISCV_CALL || type == R_RISCV_CALL_PLT);
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
            total_size = (total_size + veneer_align - 1) & ~(veneer_align - 1);
            if (total_size + nveneers * veneer_size > DRV_MOD_MAX_SIZE) {
                drvmod_free_pages(buf_pfn, DRV_MOD_BUF_ORDER);
                return -ENOEXEC;
            }
            veneer_off = kmalloc(nrela_text * sizeof(uint32_t));
            if (!veneer_off) {
                drvmod_free_pages(buf_pfn, DRV_MOD_BUF_ORDER);
                return -ENOMEM;
            }
            memset(veneer_off, 0, nrela_text * sizeof(uint32_t));
            uint32_t voff = total_size;
            for (uint32_t i = 0; i < nrela_text; i++) {
                uint32_t type = ELF_R_TYPE(rela_text[i].r_info);
                int is_call = machine == EM_AARCH64
                    ? (type == R_AARCH64_CALL26 || type == R_AARCH64_JUMP26)
                    : (type == R_RISCV_CALL || type == R_RISCV_CALL_PLT);
                if (!is_call)
                    continue;
                uint32_t symidx = ELF_R_SYM(rela_text[i].r_info);
                if (symidx >= nsyms || syms[symidx].st_shndx != 0)
                    continue;
                veneer_off[i] = voff;
                voff += veneer_size;
            }
            total_size += nveneers * veneer_size;
        }
    }

    /* LoongArch64: %got_pc_hi20/%got_pc_lo12 pairs (extern data, e.g.
     * klog_level) load the symbol address through a module-local GOT.
     * Allocate one 8-byte slot per pair at the module tail. */
    uint32_t *got_off = NULL;
    if (machine == EM_LOONGARCH && nrela_text) {
        uint32_t ngot = 0;
        for (uint32_t i = 0; i < nrela_text; i++)
            if (ELF_R_TYPE(rela_text[i].r_info) == R_LARCH_GOT_PC_HI20)
                ngot++;
        if (ngot) {
            total_size = (total_size + 7) & ~7U;
            if (total_size + ngot * 8U > DRV_MOD_MAX_SIZE) {
                drvmod_free_pages(buf_pfn, DRV_MOD_BUF_ORDER);
                return -ENOEXEC;
            }
            got_off = kmalloc(nrela_text * sizeof(uint32_t));
            if (!got_off) {
                drvmod_free_pages(buf_pfn, DRV_MOD_BUF_ORDER);
                return -ENOMEM;
            }
            memset(got_off, 0, nrela_text * sizeof(uint32_t));
            uint32_t goff = total_size;
            for (uint32_t i = 0; i < nrela_text; i++) {
                if (ELF_R_TYPE(rela_text[i].r_info) == R_LARCH_GOT_PC_HI20) {
                    got_off[i] = goff;
                    goff += 8;
                }
            }
            /* Pair every %got_pc_lo12 with the %got_pc_hi20 four bytes
             * before it (the pcalau12i + ld.d sequence).  Look the HI20
             * up by relocation offset: the list order is not guaranteed
             * to match address order. */
            for (uint32_t i = 0; i < nrela_text; i++) {
                if (ELF_R_TYPE(rela_text[i].r_info) != R_LARCH_GOT_PC_LO12)
                    continue;
                for (uint32_t j = 0; j < nrela_text; j++) {
                    if (ELF_R_TYPE(rela_text[j].r_info) == R_LARCH_GOT_PC_HI20 &&
                        rela_text[j].r_offset == rela_text[i].r_offset - 4) {
                        got_off[i] = got_off[j];
                        break;
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
            if (machine == EM_AARCH64) {
                /* ldr x16, [pc, #8]; br x16; .quad target */
                *(uint32_t *)(shadow + veneer_off[i]) = 0x58000050U;
                *(uint32_t *)(shadow + veneer_off[i] + 4) = 0xD61F0200U;
            } else {
                /* auipc t0, 0; ld t0, 16(t0); jalr zero, 0(t0); nop;
                 * .quad target */
                *(uint32_t *)(shadow + veneer_off[i]) = 0x00000297U;
                *(uint32_t *)(shadow + veneer_off[i] + 4) = 0x0102B283U;
                *(uint32_t *)(shadow + veneer_off[i] + 8) = 0x00028067U;
                *(uint32_t *)(shadow + veneer_off[i] + 12) = 0x00000013U;
            }
        }
    }
    for (uint32_t i = 0; i < nshdrs; i++) {
        elf_shdr_t *sh = &shdrs[i];
        const char *n = shstr + sh->sh_name;
        if (sh->sh_type == SHT_PROGBITS &&
            (strcmp(n, ".text") == 0 || strcmp(n, ".data") == 0 ||
             strcmp(n, ".rodata") == 0 || strcmp(n, ".sdata") == 0 ||
             strcmp(n, ".ldata") == 0)) {
            uint32_t off, sec_size;
            if (drvmod_sec_lookup(secmap, nmap, i, &off, &sec_size) < 0)
                continue;
            if (!drvmod_range_ok32(off, sec_size, total_size)) {
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
    pfn_t alloc_pfn = pfa_alloc((int)alloc_order);
    if (alloc_pfn == PFN_NONE) {
        drvmod_free_pages(shadow_pfn, alloc_order);
        drvmod_free_pages(buf_pfn, DRV_MOD_BUF_ORDER);
        return -ENOMEM;
    }
    uintptr_t load_base = PAGE_OFFSET + pfn_to_phys(alloc_pfn);

    /* RISC-V only: pre-scan PCREL_HI20 targets (LO12 references the HI20
     * instruction).  Other machines are self-contained per relocation. */
    uint32_t nslots = 0;
    uintptr_t *hi_targets = NULL;
    if (machine == EM_RISCV) {
        nslots = total_size / 8 + 1;
        hi_targets = kmalloc(nslots * sizeof(uintptr_t));
        if (!hi_targets) {
            drvmod_free_pages(alloc_pfn, alloc_order);
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
            drvmod_prescan_pcrel(rela_sets[i].rela, rela_sets[i].nrela,
                                 syms, nsyms, strtab, strtab_size,
                                 load_base, secmap, nmap, off, size,
                                 hi_targets, nslots);
        }
    }

    /* Apply relocations on the shadow, one pass per RELA section. */
    for (uint32_t i = 0; i < nrela_sets; i++) {
        uint32_t off, size;
        if (drvmod_sec_lookup(secmap, nmap, rela_sets[i].target_sec,
                              &off, &size) < 0)
            continue;
        if (drvmod_apply_reloc(machine, shadow, load_base, secmap, nmap,
                               strtab, strtab_size, syms, nsyms,
                               rela_sets[i].rela, rela_sets[i].nrela,
                               off, size, hi_targets, nslots,
                               veneer_off, got_off) == 0xFFFFFFFFU) {
            if (hi_targets)
                kfree(hi_targets);
            if (veneer_off)
                kfree(veneer_off);
            if (got_off)
                kfree(got_off);
            drvmod_free_pages(alloc_pfn, alloc_order);
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
        drvmod_free_pages(alloc_pfn, alloc_order);
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
    drvmod_free_pages(m->alloc_pfn, m->alloc_order);
    memset(m, 0, sizeof(*m));
    return 0;
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

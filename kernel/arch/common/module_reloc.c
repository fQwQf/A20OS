/* ELF instruction relocations for loadable modules.  No lifecycle,
 * allocation, or driver policy lives here. */
#include "drvmod/reloc.h"
#include "core/stdio.h"
#include "core/string.h"
#include "mm/elf.h"

static int drvmod_range_ok32(uint32_t off, uint32_t len, uint32_t cap) {
    return off <= cap && len <= cap - off;
}

/* ---- relocation type numbers ---- */

/* RISC-V */
#define R_RISCV_32 1
#define R_RISCV_64 2
#define R_RISCV_RELATIVE 3
#define R_RISCV_HI20 26
#define R_RISCV_LO12_I 27
#define R_RISCV_LO12_S 28
#define R_RISCV_BRANCH 16
#define R_RISCV_JAL 17
#define R_RISCV_CALL 18
#define R_RISCV_CALL_PLT 19
#define R_RISCV_GOT_HI20 20
#define R_RISCV_PCREL_HI20 23
#define R_RISCV_PCREL_LO12_I 24
#define R_RISCV_PCREL_LO12_S 25

/* x86-64 */
#define R_X86_64_64 1
#define R_X86_64_PC32 2
#define R_X86_64_PLT32 4
#define R_X86_64_RELATIVE 8
#define R_X86_64_32 10
#define R_X86_64_32S 11
#define R_X86_64_PC64 24

/* AArch64 */
#define R_AARCH64_ABS32 258
#define R_AARCH64_ABS64 257
#define R_AARCH64_PREL64 260
#define R_AARCH64_PREL32 261
#define R_AARCH64_JUMP26 282
#define R_AARCH64_CALL26 283
#define R_AARCH64_ADR_PREL_LO21 274
#define R_AARCH64_ADR_PREL_PG_HI21 275
#define R_AARCH64_ADD_ABS_LO12_NC 277
#define R_AARCH64_LDST_ABS_LO12_NC 278
#define R_AARCH64_RELATIVE 1027

/* LoongArch64 (numbers per the LoongArch ELF psABI) */
#define R_LARCH_32 1
#define R_LARCH_64 2
#define R_LARCH_RELATIVE 3
#define R_LARCH_ADD8 36
#define R_LARCH_SUB8 41
#define R_LARCH_ADD32 39
#define R_LARCH_SUB32 44
#define R_LARCH_B16 64
#define R_LARCH_B21 65
#define R_LARCH_B26 66
#define R_LARCH_PCALA_HI20 71
#define R_LARCH_PCALA_LO12 72
#define R_LARCH_GOT_PC_HI20 75
#define R_LARCH_GOT_PC_LO12 76
#define R_LARCH_32_PCREL 99
#define R_LARCH_RELAX 100
#define R_LARCH_64_PCREL 109
#define R_LARCH_CALL36 110

const arch_module_format_t *arch_module_format(uint32_t machine) {
    static const arch_module_format_t riscv = {
        .veneer_size = 24,
        .veneer_align = 8,
        .call_types = {R_RISCV_CALL, R_RISCV_CALL_PLT},
        .veneer_words = {0x00000297U, 0x0102B283U, 0x00028067U, 0x00000013U},
        .veneer_word_count = 4,
        .got_hi_type = R_RISCV_GOT_HI20,
        .pcrel_pairs = 1,
    };
    static const arch_module_format_t aarch64 = {
        .veneer_size = 16,
        .veneer_align = 16,
        .call_types = {R_AARCH64_CALL26, R_AARCH64_JUMP26},
        .veneer_words = {0x58000050U, 0xD61F0200U},
        .veneer_word_count = 2,
    };
    static const arch_module_format_t x86_64 = {0};
    static const arch_module_format_t loongarch64 = {
        .got_hi_type = R_LARCH_GOT_PC_HI20,
        .got_lo_type = R_LARCH_GOT_PC_LO12,
    };
    switch (machine) {
        case EM_RISCV:
            return &riscv;
        case EM_AARCH64:
            return &aarch64;
        case EM_X86_64:
            return &x86_64;
        case EM_LOONGARCH:
            return &loongarch64;
        default:
            return NULL;
    }
}

/* Bytes touched by a relocation in the target section (0 = marker/no-op). */
static uint32_t drvmod_reloc_width(uint32_t machine, uint32_t type) {
    switch (machine) {
        case EM_RISCV:
            switch (type) {
                case R_RISCV_RELATIVE:
                case R_RISCV_64:
                    return 8;
                case R_RISCV_CALL:
                case R_RISCV_CALL_PLT:
                    return 8; /* auipc + jalr pair */
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
void arch_module_prescan_pcrel(elf_rela_t *rela, uint32_t nrela,
                               elf_sym_t *syms, uint32_t nsyms,
                               const char *strtab, uint32_t strtab_size,
                               uintptr_t load_base, drvmod_secmap_t *map,
                               uint32_t nmap, uint32_t sec_load_off,
                               uint32_t sec_size, uintptr_t *hi_targets,
                               uint32_t ntargets, const uint32_t *got_off) {
    for (uint32_t i = 0; i < nrela; i++) {
        elf_rela_t *r = &rela[i];
        uint32_t type = ELF_R_TYPE(r->r_info);
        if (type != R_RISCV_PCREL_HI20 && type != R_RISCV_GOT_HI20)
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
        if (type == R_RISCV_GOT_HI20) {
            if (!got_off || !got_off[i])
                continue;
            S = load_base + got_off[i];
        } else {
            S += r->r_addend;
        }
        uint32_t load_off = sec_load_off + (uint32_t)r->r_offset;
        if (load_off < ntargets * 4)
            hi_targets[load_off / 4] = S;
    }
}

/* AArch64 ADRP: encode imm21 (page delta) into immlo/immhi fields. */
static void drvmod_a64_adrp(uint32_t *insn, int64_t delta) {
    uint32_t imm = (uint32_t)((delta >> 12) & 0x1FFFFF);
    *insn =
        (*insn & 0x9F00001F) | ((imm & 0x3) << 29) | ((imm & 0x1FFFFC) << 5);
}

/* AArch64 ADR: encode imm21 (byte delta). */
static void drvmod_a64_adr(uint32_t *insn, int64_t delta) {
    uint32_t imm = (uint32_t)(delta & 0x1FFFFF);
    *insn =
        (*insn & 0x9F00001F) | ((imm & 0x3) << 29) | ((imm & 0x1FFFFC) << 5);
}

uint32_t arch_module_apply_reloc(uint32_t machine, uint8_t *shadow,
                                 uintptr_t load_base, drvmod_secmap_t *map,
                                 uint32_t nmap, const char *strtab,
                                 uint32_t strtab_size, elf_sym_t *syms,
                                 uint32_t nsyms, elf_rela_t *rela,
                                 uint32_t nrela, uint32_t sec_load_off,
                                 uint32_t sec_size, const uintptr_t *hi_targets,
                                 uint32_t ntargets, const uint32_t *veneer_off,
                                 const uint32_t *got_off) {
    for (uint32_t i = 0; i < nrela; i++) {
        elf_rela_t *r = &rela[i];
        uint32_t type = ELF_R_TYPE(r->r_info);
        uint32_t symidx = ELF_R_SYM(r->r_info);

        uint32_t width = drvmod_reloc_width(machine, type);
        if (width == 0)
            continue; /* marker relocation (e.g. R_LARCH_RELAX) */
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
            if (drvmod_sym_addr(&syms[symidx], strtab, strtab_size, map, nmap,
                                load_base, &S) < 0)
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
                        uint32_t insn =
                            *(uint32_t *)(shadow + sec_load_off + r->r_offset);
                        uint32_t lo = (uint32_t)off & 0x1FFF;
                        insn = (insn & 0xFE0FF07F) | ((lo & 0x1F) << 7) |
                               (((lo >> 5) & 0x3F) << 25) |
                               (((lo >> 11) & 0x1) << 31);
                        *(uint32_t *)(shadow + sec_load_off + r->r_offset) =
                            insn;
                        break;
                    }
                    case R_RISCV_JAL: {
                        int32_t off = (int32_t)(S - P);
                        uint32_t insn =
                            *(uint32_t *)(shadow + sec_load_off + r->r_offset);
                        uint32_t lo = (uint32_t)off & 0x1FFFFF;
                        insn = (insn & 0xFFF) | ((lo & 0xFF000) << 1) |
                               ((lo & 0x800) << 9) | ((lo & 0x7FE) << 20) |
                               ((lo & 0x100000) << 11);
                        *(uint32_t *)(shadow + sec_load_off + r->r_offset) =
                            insn;
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
                                    printf("[DRVMOD] riscv64 call out of range "
                                           "(0x%lx)\n",
                                           (unsigned long)off64);
                                    return 0xFFFFFFFFU;
                                }
                                uintptr_t veneer = load_base + veneer_off[i];
                                off64 = (int64_t)(veneer - P);
                                if (off64 < -0x80000000LL ||
                                    off64 > 0x7fffffffLL) {
                                    printf("[DRVMOD] riscv64 veneer out of "
                                           "range (0x%lx)\n",
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
                            uint32_t *auipc =
                                (uint32_t *)(shadow + sec_load_off +
                                             r->r_offset);
                            *auipc = (*auipc & 0xFFF) | ((hi & 0xFFFFF) << 12);
                            uint32_t *jalr =
                                (uint32_t *)(shadow + sec_load_off +
                                             r->r_offset + 4);
                            *jalr = (*jalr & 0xFFFFF) | ((lo & 0xFFF) << 20);
                        }
                        break;
                    case R_RISCV_HI20: {
                        uint32_t insn =
                            *(uint32_t *)(shadow + sec_load_off + r->r_offset);
                        int32_t addend = (int32_t)((insn >> 12) & 0xFFFFF);
                        int32_t off = (int32_t)(S - (uintptr_t)addend);
                        uint32_t hi = (uint32_t)((off + 0x800) >> 12) & 0xFFFFF;
                        *(uint32_t *)(shadow + sec_load_off + r->r_offset) =
                            (insn & 0xFFF) | (hi << 12);
                        break;
                    }
                    case R_RISCV_PCREL_HI20: {
                        uint32_t insn =
                            *(uint32_t *)(shadow + sec_load_off + r->r_offset);
                        int64_t off = (int64_t)(S + r->r_addend - P);
                        if (off < -0x80000000LL || off > 0x7fffffffLL) {
                            printf("[DRVMOD] riscv64 PCREL_HI20 out of range "
                                   "(0x%lx)\n",
                                   (unsigned long)off);
                            return 0xFFFFFFFFU;
                        }
                        uint32_t hi =
                            (uint32_t)((off + 0x800LL) >> 12) & 0xFFFFF;
                        *(uint32_t *)(shadow + sec_load_off + r->r_offset) =
                            (insn & 0xFFF) | (hi << 12);
                        break;
                    }
                    case R_RISCV_GOT_HI20: {
                        if (r->r_addend != 0) {
                            printf(
                                "[DRVMOD] riscv64 GOT_HI20 nonzero addend\n");
                            return 0xFFFFFFFFU;
                        }
                        if (!got_off || !got_off[i]) {
                            printf(
                                "[DRVMOD] riscv64 GOT without slot (0x%lx)\n",
                                (unsigned long)r->r_offset);
                            return 0xFFFFFFFFU;
                        }
                        uintptr_t got = load_base + got_off[i];
                        *(uint64_t *)(shadow + got_off[i]) = S;
                        int64_t off = (int64_t)(got - P);
                        if (off < -0x80000000LL || off > 0x7fffffffLL) {
                            printf(
                                "[DRVMOD] riscv64 GOT out of range (0x%lx)\n",
                                (unsigned long)off);
                            return 0xFFFFFFFFU;
                        }
                        uint32_t insn =
                            *(uint32_t *)(shadow + sec_load_off + r->r_offset);
                        uint32_t hi =
                            (uint32_t)((off + 0x800LL) >> 12) & 0xFFFFF;
                        *(uint32_t *)(shadow + sec_load_off + r->r_offset) =
                            (insn & 0xFFF) | (hi << 12);
                        break;
                    }
                    case R_RISCV_PCREL_LO12_I:
                    case R_RISCV_PCREL_LO12_S: {
                        if (r->r_addend != 0) {
                            printf(
                                "[DRVMOD] riscv64 PCREL_LO12 nonzero addend\n");
                            return 0xFFFFFFFFU;
                        }
                        /* The target is recorded by the prescan for the HI20
                 * instruction this LO12 references (symbol st_value). */
                        uintptr_t hi_off = (uintptr_t)S - load_base;
                        uintptr_t target = 0;
                        if (hi_targets && (hi_off / 4) < ntargets)
                            target = hi_targets[hi_off / 4];
                        if (!target) {
                            printf("[DRVMOD] riscv64 PCREL_LO12 without HI20 "
                                   "(0x%lx)\n",
                                   (unsigned long)r->r_offset);
                            return 0xFFFFFFFFU;
                        }
                        uintptr_t p_hi = load_base + hi_off;
                        int32_t off = (int32_t)(target - p_hi);
                        if (type == R_RISCV_PCREL_LO12_I) {
                            uint32_t insn =
                                *(uint32_t *)(shadow + sec_load_off +
                                              r->r_offset);
                            *(uint32_t *)(shadow + sec_load_off + r->r_offset) =
                                (insn & 0xFFFFF) |
                                (((uint32_t)off & 0xFFF) << 20);
                        } else {
                            uint32_t insn =
                                *(uint32_t *)(shadow + sec_load_off +
                                              r->r_offset);
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
                        uint32_t insn =
                            *(uint32_t *)(shadow + sec_load_off + r->r_offset);
                        if (off >= -0x8000000LL && off <= 0x7FFFFFFLL) {
                            /* Direct branch. */
                            *(uint32_t *)(shadow + sec_load_off + r->r_offset) =
                                (insn & 0xFC000000) |
                                (((uint32_t)off >> 2) & 0x3FFFFFF);
                            break;
                        }
                        /* Out of ±128 MiB: route through a loader-generated veneer
                 * (ldr x16, [pc, #8]; br x16; .quad S) at the module tail. */
                        if (!veneer_off || veneer_off[i] == 0) {
                            printf("[DRVMOD] aarch64 branch out of range "
                                   "(0x%lx)\n",
                                   (unsigned long)off);
                            return 0xFFFFFFFFU;
                        }
                        uintptr_t veneer = load_base + veneer_off[i];
                        int64_t voff = (int64_t)(veneer - P);
                        *(uint32_t *)(shadow + sec_load_off + r->r_offset) =
                            (insn & 0xFC000000) |
                            (((uint32_t)voff >> 2) & 0x3FFFFFF);
                        *(uint64_t *)(shadow + veneer_off[i] + 8) =
                            S + r->r_addend;
                        break;
                    }
                    case R_AARCH64_ADR_PREL_PG_HI21: {
                        int64_t delta = (int64_t)(S + r->r_addend) -
                                        (int64_t)(P & ~0xFFFLL);
                        if (delta > 0x7FFFFFFFLL || delta < -0x80000000LL) {
                            printf(
                                "[DRVMOD] aarch64 ADRP out of range (0x%lx)\n",
                                (unsigned long)delta);
                            return 0xFFFFFFFFU;
                        }
                        drvmod_a64_adrp(
                            (uint32_t *)(shadow + sec_load_off + r->r_offset),
                            delta);
                        break;
                    }
                    case R_AARCH64_ADR_PREL_LO21: {
                        int64_t delta = (int64_t)(S + r->r_addend - P);
                        if (delta > 0xFFFFFLL || delta < -0x100000LL) {
                            printf(
                                "[DRVMOD] aarch64 ADR out of range (0x%lx)\n",
                                (unsigned long)delta);
                            return 0xFFFFFFFFU;
                        }
                        drvmod_a64_adr(
                            (uint32_t *)(shadow + sec_load_off + r->r_offset),
                            delta);
                        break;
                    }
                    case R_AARCH64_ADD_ABS_LO12_NC:
                    case R_AARCH64_LDST_ABS_LO12_NC: {
                        uint32_t insn =
                            *(uint32_t *)(shadow + sec_load_off + r->r_offset);
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
                            printf(
                                "[DRVMOD] loongarch B16 out of range (0x%lx)\n",
                                (unsigned long)off);
                            return 0xFFFFFFFFU;
                        }
                        /* beqz/bnez/bgez family and beq/bne (16-bit form):
                 * offs[15:0] at bits [25:10]; rd[4:0] must be preserved
                 * (beq/bne encode a second register there). */
                        uint32_t insn =
                            *(uint32_t *)(shadow + sec_load_off + r->r_offset);
                        *(uint32_t *)(shadow + sec_load_off + r->r_offset) =
                            (insn & 0xFC0003FF) |
                            ((((uint32_t)off >> 2) & 0xFFFF) << 10);
                        break;
                    }
                    case R_LARCH_B21: {
                        int64_t off = (int64_t)(S + r->r_addend - P);
                        if (off < -0x200000LL || off > 0x1FFFFFLL) {
                            printf(
                                "[DRVMOD] loongarch B21 out of range (0x%lx)\n",
                                (unsigned long)off);
                            return 0xFFFFFFFFU;
                        }
                        uint32_t imm = ((uint32_t)off >> 2) & 0x1FFFFF;
                        uint32_t insn =
                            *(uint32_t *)(shadow + sec_load_off + r->r_offset);
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
                                printf("[DRVMOD] loongarch B26 out of range "
                                       "(0x%lx)\n",
                                       (unsigned long)off);
                                return 0xFFFFFFFFU;
                            }
                            /* b/bl: imm26 split as [15:0]@[25:10], [20:16]@[9:5],
                     * [25:21]@[4:0] (units of 4). */
                            uint32_t imm = ((uint32_t)off >> 2) & 0x3FFFFFF;
                            uint32_t insn =
                                *(uint32_t *)(shadow + sec_load_off +
                                              r->r_offset);
                            *(uint32_t *)(shadow + sec_load_off + r->r_offset) =
                                (insn & 0xFC000000) | ((imm & 0xFFFF) << 10) |
                                (((imm >> 16) & 0x1F) << 5) |
                                ((imm >> 21) & 0x1F);
                        } else {
                            /* call36: pcaddu18i (si20 @ [24:5], 2^18 units) +
                     * jirl (imm16 @ [25:10], units of 4). */
                            if (off > 0x1FFFFFFFFLL || off < -0x200000000LL) {
                                printf("[DRVMOD] loongarch CALL36 out of range "
                                       "(0x%lx)\n",
                                       (unsigned long)off);
                                return 0xFFFFFFFFU;
                            }
                            uint32_t hi =
                                (uint32_t)((off + 0x20000) >> 18) & 0xFFFFF;
                            uint32_t lo = ((uint32_t)off >> 2) & 0xFFFF;
                            uint32_t *pc = (uint32_t *)(shadow + sec_load_off +
                                                        r->r_offset);
                            *pc = (*pc & 0xFE00001F) | (hi << 5);
                            uint32_t *jr = (uint32_t *)(shadow + sec_load_off +
                                                        r->r_offset + 4);
                            *jr = (*jr & 0xFC0003FF) | (lo << 10);
                        }
                        break;
                    }
                    case R_LARCH_GOT_PC_HI20: {
                        if (!got_off || !got_off[i]) {
                            printf(
                                "[DRVMOD] loongarch GOT without slot (0x%lx)\n",
                                (unsigned long)r->r_offset);
                            return 0xFFFFFFFFU;
                        }
                        uintptr_t got = load_base + got_off[i];
                        *(uint64_t *)(shadow + got_off[i]) = S + r->r_addend;
                        /* ld.d's imm12 is sign-extended.  When the GOT slot's low
                 * 12 bits are >= 0x800, carry into pcalau12i's page delta
                 * so the paired load still addresses the same slot. */
                        uint32_t lo = (uint32_t)got & 0xFFF;
                        uint32_t hi =
                            ((uint32_t)(got >> 12) - (uint32_t)(P >> 12) +
                             (lo >= 0x800 ? 1U : 0U)) &
                            0xFFFFF;
                        uint32_t insn =
                            *(uint32_t *)(shadow + sec_load_off + r->r_offset);
                        *(uint32_t *)(shadow + sec_load_off + r->r_offset) =
                            (insn & 0xFE00001F) | (hi << 5);
                        break;
                    }
                    case R_LARCH_GOT_PC_LO12: {
                        if (!got_off || !got_off[i]) {
                            printf(
                                "[DRVMOD] loongarch GOT without slot (0x%lx)\n",
                                (unsigned long)r->r_offset);
                            return 0xFFFFFFFFU;
                        }
                        uintptr_t got = load_base + got_off[i];
                        /* ld.d/ld.w displacements are unscaled byte offsets; the
                 * immediate carries the full low 12 bits of the slot. */
                        uint32_t imm = (uint32_t)(got & 0xFFF);
                        uint32_t insn =
                            *(uint32_t *)(shadow + sec_load_off + r->r_offset);
                        *(uint32_t *)(shadow + sec_load_off + r->r_offset) =
                            (insn & 0xFFC003FF) | (imm << 10);
                        break;
                    }
                    case R_LARCH_PCALA_HI20: {
                        uint32_t insn =
                            *(uint32_t *)(shadow + sec_load_off + r->r_offset);
                        uint32_t imm;
                        if ((insn >> 25) == 0x0D) {
                            /* pcalau12i: page-relative, and addi.d's imm12 is
                     * sign-extended, so low-12 >= 0x800 carries into hi. */
                            uint32_t lo = (uint32_t)(S + r->r_addend) & 0xFFF;
                            imm = ((uint32_t)((S + r->r_addend) >> 12) -
                                   (uint32_t)(P >> 12) +
                                   (lo >= 0x800 ? 1U : 0U)) &
                                  0xFFFFF;
                        } else {
                            /* pcaddi: byte-relative, rounded. */
                            int64_t off = (int64_t)(S + r->r_addend - P);
                            if (off > 0x7FFFFFFFLL || off < -0x80000000LL) {
                                printf("[DRVMOD] loongarch PCALA out of range "
                                       "(0x%lx)\n",
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
                        uint32_t insn =
                            *(uint32_t *)(shadow + sec_load_off + r->r_offset);
                        uint32_t prev =
                            (r->r_offset >= 4)
                                ? *(uint32_t *)(shadow + sec_load_off +
                                                r->r_offset - 4)
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
                        printf("[DRVMOD] unsupported loongarch reloc %u\n",
                               type);
                        return 0xFFFFFFFFU;
                }
                break;
            }
            default:
                printf("[DRVMOD] machine %u not supported by loader\n",
                       machine);
                return 0xFFFFFFFFU;
        }
    }
    return 0;
}

#ifndef A20_DRVMOD_RELOC_H
#define A20_DRVMOD_RELOC_H
#include "core/types.h"

/* ---- ELF structures (module files are always ELF64) ---- */

typedef struct {
    uint8_t e_ident[16];
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
    uint8_t st_info, st_other;
    uint16_t st_shndx;
    uint64_t st_value, st_size;
} elf_sym_t;

typedef struct {
    uint64_t r_offset;
    uint64_t r_info;
    int64_t r_addend;
} elf_rela_t;

#define ELF_R_TYPE(info) ((uint32_t)(info) & 0xffffffffU)
#define ELF_R_SYM(info) ((uint32_t)((info) >> 32))

typedef struct drvmod_secmap {
    uint32_t idx;
    uint32_t load_off;
    uint32_t size;
} drvmod_secmap_t;

/* Instruction encoding and pairing are owned by arch; the loader only
 * allocates and bounds-checks the resulting layout. */
typedef struct arch_module_format {
    uint32_t veneer_size, veneer_align;
    uint32_t call_types[2];
    uint32_t veneer_words[4];
    uint32_t veneer_word_count;
    uint32_t got_hi_type, got_lo_type;
    int pcrel_pairs;
} arch_module_format_t;
const arch_module_format_t *arch_module_format(uint32_t machine);
int drvmod_sym_addr(const elf_sym_t *sym, const char *strtab,
                    uint32_t strtab_size, const drvmod_secmap_t *map,
                    uint32_t nmap, uintptr_t load_base, uintptr_t *out);
void arch_module_prescan_pcrel(elf_rela_t *rela, uint32_t nrela,
                               elf_sym_t *syms, uint32_t nsyms,
                               const char *strtab, uint32_t strtab_size,
                               uintptr_t load_base, drvmod_secmap_t *map,
                               uint32_t nmap, uint32_t sec_load_off,
                               uint32_t sec_size, uintptr_t *hi_targets,
                               uint32_t ntargets, const uint32_t *got_off);
uint32_t arch_module_apply_reloc(uint32_t machine, uint8_t *shadow,
                                 uintptr_t load_base, drvmod_secmap_t *map,
                                 uint32_t nmap, const char *strtab,
                                 uint32_t strtab_size, elf_sym_t *syms,
                                 uint32_t nsyms, elf_rela_t *rela,
                                 uint32_t nrela, uint32_t sec_load_off,
                                 uint32_t sec_size, const uintptr_t *hi_targets,
                                 uint32_t ntargets, const uint32_t *veneer_off,
                                 const uint32_t *got_off);
#endif

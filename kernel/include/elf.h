#ifndef _ELF_H
#define _ELF_H

#include "types.h"

/* ============================================================
 * ELF64 Loader
 * Loads RISC-V and LoongArch64 ELF executables into a new
 * address space and prepares them for execution.
 * ============================================================ */

/* ELF magic */
#define ELF_MAGIC   0x464C457FU  /* "\x7fELF" little-endian */

/* ELF class */
#define ELFCLASS32  1
#define ELFCLASS64  2

/* ELF data encoding */
#define ELFDATA2LSB 1   /* Little endian */
#define ELFDATA2MSB 2   /* Big endian */

/* ELF file type */
#define ET_EXEC     2
#define ET_DYN      3

/* ELF machine type */
#define EM_RISCV    243
#define EM_LOONGARCH 258

/* ELF program header type */
#define PT_NULL     0
#define PT_LOAD     1
#define PT_DYNAMIC  2
#define PT_INTERP   3
#define PT_NOTE     4
#define PT_TLS      7
#define PT_PHDR     6
#define PT_GNU_STACK 0x6474e551

/* ELF segment flags */
#define PF_X  1   /* Execute */
#define PF_W  2   /* Write */
#define PF_R  4   /* Read */

/* ELF64 types */
typedef uint64_t Elf64_Addr;
typedef uint64_t Elf64_Off;
typedef uint16_t Elf64_Half;
typedef uint32_t Elf64_Word;
typedef int32_t  Elf64_Sword;
typedef uint64_t Elf64_Xword;
typedef int64_t  Elf64_Sxword;

/* ELF64 header (64 bytes) */
typedef struct {
    uint8_t     e_ident[16];
    Elf64_Half  e_type;
    Elf64_Half  e_machine;
    Elf64_Word  e_version;
    Elf64_Addr  e_entry;        /* entry point virtual address */
    Elf64_Off   e_phoff;        /* program header offset */
    Elf64_Off   e_shoff;        /* section header offset */
    Elf64_Word  e_flags;
    Elf64_Half  e_ehsize;
    Elf64_Half  e_phentsize;
    Elf64_Half  e_phnum;
    Elf64_Half  e_shentsize;
    Elf64_Half  e_shnum;
    Elf64_Half  e_shstrndx;
} Elf64_Ehdr;

/* ELF64 program header entry (56 bytes) */
typedef struct {
    Elf64_Word  p_type;
    Elf64_Word  p_flags;
    Elf64_Off   p_offset;       /* offset in file */
    Elf64_Addr  p_vaddr;        /* virtual address */
    Elf64_Addr  p_paddr;        /* physical address (ignored) */
    Elf64_Xword p_filesz;       /* size in file */
    Elf64_Xword p_memsz;        /* size in memory */
    Elf64_Xword p_align;
} Elf64_Phdr;

/* ---- Load result ---- */
typedef struct elf_load_info {
    uint64_t  entry;
    uint64_t  base;
    uint64_t  end_va;
    uint64_t  stack_top;
    uint64_t  brk;
    uint64_t  phdr_va;
    uint32_t  phnum;
    uint32_t  phentsize;
    uint64_t  load_addr;
    size_t    load_size;
    uint64_t *pgdir;
    uint64_t  tls_va;
    uint64_t  tls_size;
    uint64_t  tls_tp;
} elf_load_info_t;

/* ---- API ---- */

/* Load an ELF file from the filesystem (fd) into current process page tables.
 * Returns 0 on success, negative errno on failure.
 * On success, fills *info with entry point and memory layout. */
int elf_load(int fd, elf_load_info_t *info);

/* Load an ELF from a memory buffer (for embedded init binary) */
int elf_load_from_buf(const void *buf, size_t len, elf_load_info_t *info);

/* Verify ELF header sanity */
int elf_check_header(const Elf64_Ehdr *eh);

/* Build initial user stack with argc/argv/envp/auxv.
 * Returns new sp value. */
uint64_t elf_setup_stack(uint64_t stack_top, int argc, char *const argv[],
                          char *const envp[], const elf_load_info_t *info);

#endif /* _ELF_H */

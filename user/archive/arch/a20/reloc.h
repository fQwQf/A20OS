#ifndef _A20_RELOC_H
#define _A20_RELOC_H

#define R_A20_RELATIVE  3
#define R_A20_64        2
#define R_A20_JUMP_SLOT 5
#define R_A20_GLOB_DAT  6

#define R_A20_COPY      0
#define R_A20_TLS_DTPMOD64  7
#define R_A20_TLS_DTPREL64  8
#define R_A20_TLS_TPREL64   9

#define REL_SYMBOLIC    R_A20_64
#define REL_OFFSET      0
#define REL_GOT         R_A20_GLOB_DAT
#define REL_PLT         R_A20_JUMP_SLOT
#define REL_RELATIVE    R_A20_RELATIVE
#define REL_COPY        R_A20_COPY
#define REL_DTPMOD      R_A20_TLS_DTPMOD64
#define REL_TPREL       R_A20_TLS_TPREL64
#define REL_TPREL_NEG   0
#define REL_DTPREL      R_A20_TLS_DTPREL64

#define CRTJMP(pc, sp) __asm__ volatile( \
    "mv sp, %1\n" \
    "jr %0\n" \
    : : "r"(pc), "r"(sp) : "memory" )

#endif

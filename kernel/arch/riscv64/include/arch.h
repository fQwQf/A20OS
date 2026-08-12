#ifndef _ARCH_RISCV64_H
#define _ARCH_RISCV64_H

#define ARCH_HAS_VDSO 1

#include "platform.h"
#include "console.h"
#include "cpu.h"
#include "page_table.h"
#include "trap_frame.h"
#include "firmware.h"

uint32_t riscv64_asid_alloc(void);
void riscv64_asid_release(uint32_t asid);

#define ARCH_MM_CONTEXT_ALLOC() riscv64_asid_alloc()
#define ARCH_MM_CONTEXT_RELEASE(asid) riscv64_asid_release(asid)
#define ARCH_MM_ADDRESS_SPACE_TOKEN(pgdir, asid) \
    arch_make_satp_asid((pgdir), (asid))

#endif

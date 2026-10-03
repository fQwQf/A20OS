#ifndef _ARCH_PPC64LE_H
#define _ARCH_PPC64LE_H

#define ARCH_HAS_VDSO 1

#include "platform.h"
#include "console.h"
#include "cpu.h"
#include "page_table.h"
#include "trap_frame.h"
#include "firmware.h"


/* an instruction storage fault reaches the external vector
 * with SRR0 clobbered, so demand paging can never map the page. */
#define ARCH_INSN_FAULT_UNRECOVERABLE 1

#endif

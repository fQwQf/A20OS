#include "mlibc-asm/helpers.h"

PROC_START(_start)
	/* Native A20 programs carry start_info in a0.  Preserve it while the
	 * interpreter consumes the conventional initial stack in sp. */
	mv s0, a0
	mv s1, sp
	call relocateSelf

	mv a0, s1
	call interpreterMain

	mv t0, a0
	mv a0, s0
	mv sp, s1
	jr t0
PROC_END(_start)

GNU_STACK_NOTE()

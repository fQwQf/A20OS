#ifndef _CORE_ERRNO_H
#define _CORE_ERRNO_H

/*
 * Kernel-internal errno namespace.
 *
 * Values intentionally match the Linux ABI today because most in-kernel
 * subsystems return Linux-compatible negative errno values. Keeping the
 * definitions under core/ prevents common code from depending on a selected
 * userspace ABI header bundle.
 */
#include "../abi/linux/errno.h"

#endif /* _CORE_ERRNO_H */

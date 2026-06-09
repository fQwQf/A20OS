#ifndef _ABI_CURRENT_H
#define _ABI_CURRENT_H

/*
 * This header selects userspace ABI definitions only. Kernel-internal
 * compatibility constants live under core/ so common subsystems do not depend
 * on the active userspace ABI bundle.
 */

#if defined(CONFIG_ABI_LINUX) || defined(CONFIG_ABI_BOTH)
# include "abi/linux/futex.h"
# include "abi/linux/ioctl.h"
# include "abi/linux/mman.h"
# include "abi/linux/poll.h"
# include "abi/linux/resource.h"
#endif

#if defined(CONFIG_ABI_NATIVE) || defined(CONFIG_ABI_BOTH)
# include "abi/native/types.h"
# include "abi/native/errno.h"
# include "abi/native/rights.h"
# include "abi/native/syscall_nr.h"
# include "abi/native/startup.h"
#endif

#if !defined(CONFIG_ABI_LINUX) && !defined(CONFIG_ABI_NATIVE) && !defined(CONFIG_ABI_BOTH)
# error "No user ABI selected"
#endif

#endif
